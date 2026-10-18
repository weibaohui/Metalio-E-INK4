// main/services/dynamic_apps/dynamic_app_loader.cc
// 动态应用加载器（包格式：目录制，惰性加载）：
//
//   包格式：/sdcard/metalio/e-ink/apps/<dir>/
//     manifest.json  元数据 {id, name, icon?, abi, version?}
//     app.so         代码（dlopen 加载）
//     icon.png       可选自定义图标（S: FS 路径直用）
//
//   开机/插卡扫描：只读 manifest.json（cJSON 解析，无代码执行）——abi 闸门在
//   扫描阶段拒绝不匹配的包（固件升级后旧应用点名拒绝，不崩、不执行代码）；
//   点开：dlopen(app.so) → entry(&desc) → desc.create() → 屏幕，abi 二次校验，
//   挂 LV_EVENT_DELETE 卸载钩子；退出：屏幕树删除完成后（lv_async_call）dlclose
//   → esp_elf_deinit 释放 PSRAM 代码段，真实卸载。
//
// 契约（见 dynamic_app_abi.h）：
//   entry 必须无副作用（仅点开时调用，仅填 desc）；create 返回
//   lv_obj_create(NULL) 新建的屏幕；应用定时器/任务必须在屏幕 LV_EVENT_DELETE
//   里自行清理——dlclose 后 .so 代码段被释放，跨屏幕的后台定时器/任务存活即悬空。
//
// 符号表机制：ESP_ELFSYM_EXPORT(name) 的地址由链接器在链接期解析，天生自洽；
// 新 .so 引用了表里没有的符号 → dlopen(RTLD_NOW) 失败并在 dlerror 里点名，
// 把该符号加进本表重编固件即可。
#include "dynamic_app_loader.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_elf.h>
#include <esp_dlfcn.h>
#include <esp_random.h>
#include <lvgl.h>
#include <soc/soc.h>

#include "core/async_settings_store.h"
#include "core/app_manager.h"
#include "display/screen/home_screen/home_screen.h"
#include "display/screen/vk_key_handler.h"
#include "display/font/fontpack_lvgl.h"
#include "dynamic_app_shims.h"
#include "sd_paths.h"

#include "dynamic_app_abi.h"

namespace dynamic_apps {
namespace {

constexpr const char *TAG = "DynApp";
constexpr const char *kManifestName = "manifest.json";
constexpr const char *kSoName = "app.so";

// 供 .so 解析的固件符号表。扩展方式：xtensa-esp32s3-elf-nm 新 .so | grep ' U '，
// 把缺失符号加进来重编固件（表外符号 dlopen 时由 dlerror 点名）。
const struct esp_elfsym g_metalio_dynapp_syms[] = {
    // LVGL 核心（hello 起步集）
    ESP_ELFSYM_EXPORT(lv_obj_create),
    ESP_ELFSYM_EXPORT(lv_label_create),
    ESP_ELFSYM_EXPORT(lv_label_set_text),
    ESP_ELFSYM_EXPORT(lv_obj_center),
    ESP_ELFSYM_EXPORT(lv_obj_align),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_text_color),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_text_font),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_bg_color),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_bg_opa),
    ESP_ELFSYM_EXPORT(lv_color_white),
    ESP_ELFSYM_EXPORT(lv_color_black),
    // LVGL 布局/样式/事件（真实应用面）
    ESP_ELFSYM_EXPORT(lv_obj_remove_style_all),
    ESP_ELFSYM_EXPORT(lv_obj_set_pos),
    ESP_ELFSYM_EXPORT(lv_obj_set_size),
    ESP_ELFSYM_EXPORT(lv_obj_set_width),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_radius),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_border_width),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_border_color),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_text_align),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_pad_all),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_pad_row),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_pad_ver),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_pad_hor),
    ESP_ELFSYM_EXPORT(lv_obj_set_style_margin_ver),
    ESP_ELFSYM_EXPORT(lv_obj_set_flex_grow),
    ESP_ELFSYM_EXPORT(lv_obj_set_flex_flow),
    ESP_ELFSYM_EXPORT(lv_obj_set_flex_align),
    ESP_ELFSYM_EXPORT(lv_obj_add_flag),
    ESP_ELFSYM_EXPORT(lv_obj_remove_flag),
    ESP_ELFSYM_EXPORT(lv_obj_clear_flag),
    ESP_ELFSYM_EXPORT(lv_obj_add_event_cb),
    ESP_ELFSYM_EXPORT(lv_event_get_user_data),
    ESP_ELFSYM_EXPORT(lv_event_get_code),
    ESP_ELFSYM_EXPORT(lv_indev_active),
    ESP_ELFSYM_EXPORT(lv_indev_get_point),
    ESP_ELFSYM_EXPORT(lv_indev_reset),
    ESP_ELFSYM_EXPORT(lv_indev_wait_release),
    ESP_ELFSYM_EXPORT(lv_indev_get_gesture_dir),
    ESP_ELFSYM_EXPORT(lv_label_set_long_mode),
    ESP_ELFSYM_EXPORT(lv_palette_darken),
    ESP_ELFSYM_EXPORT(lv_palette_lighten),
    ESP_ELFSYM_EXPORT(lv_color_hex),
    ESP_ELFSYM_EXPORT(lv_pct),
    ESP_ELFSYM_EXPORT(lv_obj_delete),
    // libc 补充（组件 libc 表之外：SD 文件读写、snprintf/strncmp/strncpy、随机数）
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strncpy),
    // 定时器与异步
    ESP_ELFSYM_EXPORT(lv_timer_create),
    ESP_ELFSYM_EXPORT(lv_timer_delete),
    ESP_ELFSYM_EXPORT(lv_timer_pause),
    ESP_ELFSYM_EXPORT(lv_timer_resume),
    ESP_ELFSYM_EXPORT(lv_timer_set_period),
    ESP_ELFSYM_EXPORT(lv_async_call),
    // 字体
    ESP_ELFSYM_EXPORT(fontpack_lv_font_get),
    ESP_ELFSYM_EXPORT(fontpack_lv_font_ui),
    // libc 补充（组件 libc 表之外：SD 文件读写与随机数）
    ESP_ELFSYM_EXPORT(fopen),
    ESP_ELFSYM_EXPORT(fclose),
    ESP_ELFSYM_EXPORT(fread),
    ESP_ELFSYM_EXPORT(fwrite),
    ESP_ELFSYM_EXPORT(fprintf),
    ESP_ELFSYM_EXPORT(fscanf),
    ESP_ELFSYM_EXPORT(fgets),
    ESP_ELFSYM_EXPORT(fputs),
    ESP_ELFSYM_EXPORT(fseek),
    ESP_ELFSYM_EXPORT(ftell),
    ESP_ELFSYM_EXPORT(fflush),
    ESP_ELFSYM_EXPORT(esp_random),
    // cJSON 接口（供应用免 NVS 解析与生成配置文件）
    ESP_ELFSYM_EXPORT(cJSON_Parse),
    ESP_ELFSYM_EXPORT(cJSON_Delete),
    ESP_ELFSYM_EXPORT(cJSON_Print),
    ESP_ELFSYM_EXPORT(cJSON_PrintUnformatted),
    ESP_ELFSYM_EXPORT(cJSON_CreateObject),
    ESP_ELFSYM_EXPORT(cJSON_CreateArray),
    ESP_ELFSYM_EXPORT(cJSON_CreateString),
    ESP_ELFSYM_EXPORT(cJSON_CreateNumber),
    ESP_ELFSYM_EXPORT(cJSON_CreateBool),
    ESP_ELFSYM_EXPORT(cJSON_AddItemToObject),
    ESP_ELFSYM_EXPORT(cJSON_AddStringToObject),
    ESP_ELFSYM_EXPORT(cJSON_AddNumberToObject),
    ESP_ELFSYM_EXPORT(cJSON_AddBoolToObject),
    ESP_ELFSYM_EXPORT(cJSON_GetObjectItemCaseSensitive),
    ESP_ELFSYM_EXPORT(cJSON_GetStringValue),
    ESP_ELFSYM_EXPORT(cJSON_IsString),
    ESP_ELFSYM_EXPORT(cJSON_IsNumber),
    ESP_ELFSYM_EXPORT(cJSON_IsBool),
    // C ABI 垫片：C++ 固件设施的最小导出面
    ESP_ELFSYM_EXPORT(dynapp_screen_status_bar),
    ESP_ELFSYM_EXPORT(dynapp_vk_attach),
    ESP_ELFSYM_EXPORT(dynapp_lv_async),
    ESP_ELFSYM_EXPORT(dynapp_screen_go_home),
    ESP_ELFSYM_EXPORT(dynapp_epd_full_refresh),
    ESP_ELFSYM_EXPORT(dynapp_get_bundle_path),
    ESP_ELFSYM_EXPORT(dynapp_get_data_path),
    ESP_ELFSYM_EXPORT(dynapp_fopen_data),
    ESP_ELFSYM_EXPORT(dynapp_is_path_allowed),
    ESP_ELFSYM_EXPORT(dynapp_power_sleep),
    ESP_ELFSYM_EXPORT(dynapp_power_reboot),
    ESP_ELFSYM_EXPORT(dynapp_power_shutdown),
    ESP_ELFSYM_EXPORT(dynapp_is_network_ready),
    ESP_ELFSYM_EXPORT(dynapp_time_is_synced),
    ESP_ELFSYM_EXPORT(dynapp_time_last_sync_epoch),
    ESP_ELFSYM_EXPORT(dynapp_time_get_tz_offset_min),
    ESP_ELFSYM_EXPORT(dynapp_time_set_tz_offset_min),
    ESP_ELFSYM_EXPORT(dynapp_time_sync_now),
    ESP_ELFSYM_EXPORT(dynapp_time_set_manual),
    ESP_ELFSYM_EXPORT(dynapp_toast_show),
    ESP_ELFSYM_END
};

using DynAppEntryFn = void (*)(metalio_app_desc_t *);

// dlopen 路径换算：组件按 FS_PATH("/sdcard"，sdkconfig ELF_FILE_SYSTEM_BASE_PATH)
// + "/" + name 拼接打开，因此必须传卡内相对路径（剥掉挂载点前缀）。
std::string DlopenPath(const std::string &abs_path) {
    return SdUserPath(abs_path.c_str());
}

// 包内路径安全性：拒绝 ".."、绝对路径与反斜杠，防 manifest 图标字段穿越。
bool IconRelPathSafe(const char *rel) {
    if (rel == nullptr || rel[0] == '\0') return false;
    if (rel[0] == '/' || strchr(rel, '\\') != nullptr) return false;
    if (strstr(rel, "..") != nullptr) return false;
    return true;
}

// 加载失败占位屏：绝不把 NULL 喂给 ScreenLoadReplace（NULL 控件 = 无限重绘假死）。
// 必须挂入虚拟键栈：否则按键分发器仍认为活跃屏是"home"，HOME/返回全部 no-op，用户被卡死在本屏。
lv_obj_t *MakeFailScreen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "App load failed\ncheck serial log");
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    VkKeyScreenDesc desc;  // 空描述：HOME/vk_prev 走默认策略返回桌面
    VkKey_AttachScreen(scr, "dynapp_fail", desc);
    return scr;
}

// ESP32-S3 PSRAM 动态加载：若函数指针落在 PSRAM D-Bus（0x3C000000..0x3E000000），
// CPU 从 D-Bus 取指会触发 InstructionFetchError (EXCCAUSE=2) 重启崩溃回桌面。
// 修正为 S3 的 I-Bus 镜像空间（0x42000000..0x44000000）。
template <typename T>
T EnsureIbusCallable(T fn_ptr) {
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_ELF_LOADER_LOAD_PSRAM
    uintptr_t addr = reinterpret_cast<uintptr_t>(fn_ptr);
    if (addr >= SOC_DROM_LOW && addr < SOC_DROM_HIGH) {
        addr += (SOC_IROM_LOW - SOC_DROM_LOW);
        return reinterpret_cast<T>(addr);
    }
#endif
    return fn_ptr;
}

// dlopen 并校验描述符。成功返回 true 且 *out_handle 有效（调用方负责 dlclose 或挂卸载钩子）；
// 失败已 dlclose 收尾，返回 false。
bool ReadDesc(const char *path, void **out_handle, metalio_app_desc_t *out_desc) {
    void *handle = dlopen(path, RTLD_NOW);
    if (handle == nullptr) {
        ESP_LOGE(TAG, "dlopen %s failed: %s", path, dlerror());
        return false;
    }

    DynAppEntryFn entry = reinterpret_cast<DynAppEntryFn>(dlsym(handle, "metalio_app_entry"));
    if (entry == nullptr) {
        ESP_LOGE(TAG, "%s: metalio_app_entry not found: %s", path, dlerror());
        dlclose(handle);
        return false;
    }
    entry = EnsureIbusCallable(entry);

    memset(out_desc, 0, sizeof(*out_desc));
    entry(out_desc);

    out_desc->create = EnsureIbusCallable(out_desc->create);

    if (out_desc->abi_version != METALIO_DYNAPP_ABI_VERSION) {
        ESP_LOGE(TAG, "%s: abi %u != %u", path, out_desc->abi_version, METALIO_DYNAPP_ABI_VERSION);
        dlclose(handle);
        return false;
    }
    if (out_desc->id == nullptr || out_desc->name == nullptr || out_desc->create == nullptr) {
        ESP_LOGE(TAG, "%s: incomplete desc (id/name/create null)", path);
        dlclose(handle);
        return false;
    }

    *out_handle = handle;
    return true;
}

// 屏幕删除事件 → 投递 async 卸载。删除事件期间子对象析构仍引用 .so 代码，
// 必须等到本轮删除完成（下一 LVGL 周期）再 dlclose。
// 注：从删除事件到 async 回调仅隔一个 LVGL 周期（毫秒级），e-ink 上人类
// 无法在该窗口内重新点开同一应用，无需在用计数。
struct ScreenDeleteCtx {
    std::string so_path;
    std::string bundle_dir;
};

// 屏幕删除事件 → 投递 async 卸载。删除事件期间子对象析构仍引用 .so 代码，
// 必须等到本轮删除完成（下一 LVGL 周期）再 dlclose。
void OnDynScreenDelete(lv_event_t *e) {
    auto *ctx = static_cast<ScreenDeleteCtx *>(lv_event_get_user_data(e));
    lv_async_call([](void *p) {
        auto *c = static_cast<ScreenDeleteCtx *>(p);
        // dlopen 已加载的 .so 返回同句柄；dlclose 即真实卸载（esp_elf_deinit 释放代码段）。
        void *h = dlopen(DlopenPath(c->so_path).c_str(), RTLD_NOW);
        if (h != nullptr) {
            dlclose(h);
            ESP_LOGI(TAG, "dynamic app unloaded: %s", c->so_path.c_str());
        }
        dynapp_clear_active_bundle();
        delete c;
    }, ctx);
}

// 惰性动态应用模块：持有 manifest 元数据、bundle 目录与可执行文件路径；点开时设置沙箱上下文并 dlopen 创建屏幕，
// 屏幕销毁后 dlclose 卸载并清理沙箱上下文。
class LazyDynamicAppModule : public AppModule {
public:
    LazyDynamicAppModule(std::string id, std::string name, std::string icon, std::string bundle_dir, std::string so_path)
        : id_(std::move(id)), name_(std::move(name)), icon_(std::move(icon)),
          bundle_dir_(std::move(bundle_dir)), so_path_(std::move(so_path)) {}

    const char *GetId() const override { return id_.c_str(); }
    const char *GetName() const override { return name_.c_str(); }
    const char *GetIconPath() const override { return icon_.c_str(); }
    bool IsMandatory() const override { return false; }
    lv_obj_t *CreateUI(lv_obj_t * /*parent*/ = nullptr) override {
        void *handle = nullptr;
        metalio_app_desc_t desc = {};

        // 激活当前应用沙箱上下文（bundle 路径及自动生成的 data 目录）
        dynapp_set_active_bundle(bundle_dir_.c_str());

        if (!ReadDesc(DlopenPath(so_path_).c_str(), &handle, &desc)) {
            dynapp_clear_active_bundle();
            return MakeFailScreen();
        }
        lv_obj_t *scr = static_cast<lv_obj_t *>(desc.create());
        if (scr == nullptr) {
            ESP_LOGE(TAG, "%s: create returned null", so_path_.c_str());
            dlclose(handle);
            dynapp_clear_active_bundle();
            return MakeFailScreen();
        }
        // 卸载钩子交给屏幕：树删除完成后 async dlclose 并清理沙箱上下文
        auto *del_ctx = new ScreenDeleteCtx{so_path_, bundle_dir_};
        lv_obj_add_event_cb(scr, OnDynScreenDelete, LV_EVENT_DELETE, del_ctx);
        ESP_LOGI(TAG, "dynamic app loaded: %s (%s, bundle: %s)",
                 name_.c_str(), so_path_.c_str(), bundle_dir_.c_str());
        return scr;
    }
    ScreenFactory GetScreenFactory() const override { return nullptr; }

private:
    std::string id_;
    std::string name_;
    std::string icon_;
    std::string bundle_dir_;
    std::string so_path_;
};

// 读整个文本文件到 string；失败返回空串。
std::string ReadFileText(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return {};
    }
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > 4096) break;  // manifest 上限
    }
    fclose(f);
    return out;
}

// manifest 解析结果（abi/id/name 校验通过 + 可执行文件存在才有效）
struct ManifestInfo {
    bool valid = false;
    int abi = 0;
    std::string id, name, icon_rel, version, executable;
};

// 读 manifest（无代码执行）：解析字段并校验 abi/id/name 与可执行文件存在性。
bool ParseManifestDir(const std::string &dir_path, ManifestInfo &out) {
    const std::string manifest_path = dir_path + "/" + kManifestName;
    const std::string text = ReadFileText(manifest_path.c_str());
    if (text.empty()) {
        ESP_LOGD(TAG, "%s: no manifest, skip", manifest_path.c_str());
        return false;
    }

    cJSON *root = cJSON_Parse(text.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "%s: manifest parse failed", manifest_path.c_str());
        return false;
    }

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (item->string == nullptr) continue;
        if (strcmp(item->string, "id") == 0 && cJSON_IsString(item)) {
            out.id = item->valuestring;
        } else if (strcmp(item->string, "name") == 0 && cJSON_IsString(item)) {
            out.name = item->valuestring;
        } else if (strcmp(item->string, "icon") == 0 && cJSON_IsString(item)) {
            out.icon_rel = item->valuestring;
        } else if (strcmp(item->string, "version") == 0 && cJSON_IsString(item)) {
            out.version = item->valuestring;
        } else if (strcmp(item->string, "executable") == 0 && cJSON_IsString(item)) {
            out.executable = item->valuestring;
        } else if (strcmp(item->string, "abi") == 0 && cJSON_IsNumber(item)) {
            out.abi = static_cast<int>(item->valuedouble);
        }
    }
    cJSON_Delete(root);

    if (out.abi != METALIO_DYNAPP_ABI_VERSION) {
        // 扫描阶段闸门：abi 不匹配的包点名拒绝（固件升级后旧包在此失效，不执行代码）
        ESP_LOGE(TAG, "%s: abi %d != %u, skip", manifest_path.c_str(), out.abi, METALIO_DYNAPP_ABI_VERSION);
        return false;
    }
    if (out.id.empty() || out.id.size() > 12) {
        // NVS 键为 "in_<id>"/"en_<id>"，总长 ≤15
        ESP_LOGE(TAG, "%s: id '%s' invalid (need 1..12 chars)", manifest_path.c_str(), out.id.c_str());
        return false;
    }
    if (out.name.empty()) {
        ESP_LOGE(TAG, "%s: name empty", manifest_path.c_str());
        return false;
    }

    // 强制规范：动态库固定放置在 lib/app.so（或 manifest 显式指定的唯一路径），不设多层猜测兼容
    const std::string so_rel = out.executable.empty() ? "lib/app.so" : out.executable;
    const std::string so_path = dir_path + "/" + so_rel;
    struct stat st = {};
    if (stat(so_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "%s: mandatory executable '%s' not found, rejected", manifest_path.c_str(), so_path.c_str());
        return false;
    }
    out.executable = so_path;

    // 自动确保应用私有 data 目录存在，重装/升级时绝不覆盖 data
    SdEnsureDir((dir_path + "/data").c_str());

    out.valid = true;
    return true;
}

// 注册有效包（不做安装态判断）。已注册返回 0，新注册返回 1。
int RegisterDir(const std::string &dir_path, const ManifestInfo &info) {
    if (AppManager::GetInstance().GetAppById(info.id.c_str()) != nullptr) {
        // 动态应用先于内置应用注册，同名时内置后注册会替换动态入口（内置优先）
        ESP_LOGI(TAG, "dynamic app %s already registered, skip", info.id.c_str());
        return 0;
    }

    // 图标：S: 盘符（LVGL POSIX FS → /sdcard）+ 卡内相对路径；缺省用内置图标
    std::string icon = "A:ic_app_home_clock.spng";
    if (IconRelPathSafe(info.icon_rel.c_str())) {
        icon = std::string("S:/") + SdUserPath(dir_path.c_str()) + "/" + info.icon_rel;
    } else if (!info.icon_rel.empty()) {
        ESP_LOGW(TAG, "%s: unsafe icon path '%s', using builtin", dir_path.c_str(), info.icon_rel.c_str());
    }

    auto module = std::make_shared<LazyDynamicAppModule>(
        info.id, info.name, icon, dir_path, info.executable);
    AppManager::GetInstance().RegisterApp(module);
    ESP_LOGI(TAG, "dynamic app available: %s (%s) [bundle: %s] — lazy, loads on tap",
             module->GetName(), module->GetId(), dir_path.c_str());
    return 1;
}

// 开机/推送路径：仅当安装标志为真（缺省真）时注册；未安装的包留在商店。
int RegisterDirIfInstalled(const std::string &dir_path) {
    ManifestInfo info;
    if (!ParseManifestDir(dir_path, info)) {
        return 0;
    }
    const std::string flag_key = "in_" + info.id;
    if (!AsyncSettingsStore::GetInstance().GetBool("dynapp", flag_key.c_str(), true)) {
        ESP_LOGI(TAG, "dynamic app %s not installed (store only), skip", info.id.c_str());
        return 0;
    }
    return RegisterDir(dir_path, info);
}
}  // namespace

void RegisterElfSymbols() {
    static bool registered = false;
    if (registered) {
        return;
    }
    int err = esp_elf_register_symbol(g_metalio_dynapp_syms);
    registered = (err == 0);
    ESP_LOGI(TAG, "firmware symbol table registered: %d", err);
}

void ScanAndRegister() {
    // 自确保目录（开机扫描时 SdEnsureAppLayout 未必被其他服务调过）
    SdEnsureDir(SD_PATH_APPS);
    DIR *dir = opendir(SD_PATH_APPS);
    if (dir == nullptr) {
        ESP_LOGI(TAG, "no dynamic app dir %s (SD not mounted or empty)", SD_PATH_APPS);
        return;
    }

    std::vector<std::string> dirs;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const std::string dir_path = std::string(SD_PATH_APPS) + "/" + ent->d_name;
        struct stat st = {};
        if (stat(dir_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            dirs.push_back(dir_path);
        }
    }
    closedir(dir);

    // 优先排序：.app 规范后缀目录排在最前
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b) {
        auto has_app_ext = [](const std::string &s) {
            return s.size() >= 4 && s.compare(s.size() - 4, 4, ".app") == 0;
        };
        const bool a_app = has_app_ext(a);
        const bool b_app = has_app_ext(b);
        if (a_app != b_app) {
            return a_app > b_app;
        }
        return a < b;
    });

    int registered_new = 0;
    for (const auto &d : dirs) {
        registered_new += RegisterDirIfInstalled(d);
    }

    if (!dirs.empty()) {
        ESP_LOGI(TAG, "dynamic app scan done: %u dirs under %s", (unsigned)dirs.size(), SD_PATH_APPS);
    }
    if (registered_new > 0) {
        // 有人正停在桌面时让新应用即时出现（内部走线程安全投递，不在首页则 no-op）
        HomeScreenRequestAppsRefresh();
    }
}


std::vector<DynPackage> ListPackages() {
    std::vector<DynPackage> out;
    SdEnsureDir(SD_PATH_APPS);
    DIR *dir = opendir(SD_PATH_APPS);
    if (dir == nullptr) {
        return out;
    }
    std::vector<std::string> dirs;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const std::string dir_path = std::string(SD_PATH_APPS) + "/" + ent->d_name;
        struct stat st = {};
        if (stat(dir_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            dirs.push_back(dir_path);
        }
    }
    closedir(dir);

    // 优先排序：.app 规范后缀目录排在最前
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b) {
        auto has_app_ext = [](const std::string &s) {
            return s.size() >= 4 && s.compare(s.size() - 4, 4, ".app") == 0;
        };
        const bool a_app = has_app_ext(a);
        const bool b_app = has_app_ext(b);
        if (a_app != b_app) {
            return a_app > b_app;
        }
        return a < b;
    });

    std::vector<std::string> seen_ids;
    for (const auto &d : dirs) {
        ManifestInfo info;
        if (!ParseManifestDir(d, info)) {
            continue;
        }
        bool seen = false;
        for (const auto &sid : seen_ids) {
            if (sid == info.id) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        seen_ids.push_back(info.id);

        DynPackage pkg;
        pkg.dir = d;
        pkg.id = info.id;
        pkg.name = info.name;
        pkg.version = info.version;
        pkg.installed = (AppManager::GetInstance().GetAppById(info.id.c_str()) != nullptr);
        out.push_back(std::move(pkg));
    }
    return out;
}

bool InstallPackage(const std::string& id) {
    auto pkgs = ListPackages();
    for (const auto &pkg : pkgs) {
        if (pkg.id == id) {
            ManifestInfo info;
            if (!ParseManifestDir(pkg.dir, info)) {
                return false;
            }
            RegisterDir(pkg.dir, info);
            AsyncSettingsStore::GetInstance().SetBool("dynapp", ("in_" + info.id).c_str(), true);
            ESP_LOGI(TAG, "package installed: %s (%s, bundle: %s)",
                     info.name.c_str(), info.id.c_str(), pkg.dir.c_str());
            return true;
        }
    }
    ESP_LOGW(TAG, "install: package %s not found on SD", id.c_str());
    return false;
}

bool UninstallPackage(const std::string& id) {
    AsyncSettingsStore::GetInstance().SetBool("dynapp", ("in_" + id).c_str(), false);
    const bool was = AppManager::GetInstance().UnregisterApp(id.c_str());
    if (was) {
        ESP_LOGI(TAG, "package uninstalled: %s", id.c_str());
    }
    return true;
}

}  // namespace dynamic_apps
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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// libgcc intrinsics required by 64-bit int and double operations in dynamic apps
// (双精度浮点全族导出:第三方应用做任何浮点运算都不再撞"符号缺失"墙)
extern "C" {
void __adddf3(void);
void __divdf3(void);
void __divdi3(void);
void __eqdf2(void);
void __extendsfdf2(void);
void __floatsidf(void);
void __floatundidf(void);
void __floatunsidf(void);
void __gedf2(void);
void __gtdf2(void);
void __ledf2(void);
void __ltdf2(void);
void __moddi3(void);
void __muldf3(void);
void __muldi3(void);
void __nedf2(void);
void __negdf2(void);
void __powidf2(void);
void __subdf3(void);
void __udivdi3(void);
void __umoddi3(void);
void __unorddf2(void);
}

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
#include "services/radio/radio_player.h"
#include "services/radio/radio_catalog.h"
#include "display/font/fontpack_lvgl.h"
#include "dynamic_app_shims.h"
#include "sd_paths.h"

#include "dynamic_app_abi.h"
#include <unistd.h>
#include <esp_heap_caps.h>
#include "reader_core/zip_reader.h"
#include "reader/http_download_file.h"
#include "SdCardManager.hpp"
#include "mcp_server.h"

namespace dynamic_apps {
namespace {

constexpr const char *TAG = "DynApp";
constexpr const char *kManifestName = "manifest.json";
constexpr const char *kSoName = "app.so";

// 供 .so 解析的固件符号表。由 scripts/gen_symbols.sh 从固件 ELF 自动生成
// (lv_* / dynapp_* 前缀全量 + 基础库清单),禁止手改;重编固件后运行该脚本更新。
// 表外符号 dlopen 时由 dlerror 点名。
const struct esp_elfsym g_metalio_dynapp_syms[] = {
    // libgcc 内建函数(64 位整数与双精度浮点全族;extern 声明见文件头)。
    // 若某符号未被固件链接,固件链接期会报 undefined reference —— 按报错
    // 从本清单与 gen_symbols.sh 的 LIBGCC_SYMBOLS 中同步剔除即可。
    ESP_ELFSYM_EXPORT(__adddf3),
    ESP_ELFSYM_EXPORT(__divdf3),
    ESP_ELFSYM_EXPORT(__divdi3),
    ESP_ELFSYM_EXPORT(__eqdf2),
    ESP_ELFSYM_EXPORT(__extendsfdf2),
    ESP_ELFSYM_EXPORT(__floatsidf),
    ESP_ELFSYM_EXPORT(__floatundidf),
    ESP_ELFSYM_EXPORT(__floatunsidf),
    ESP_ELFSYM_EXPORT(__gedf2),
    ESP_ELFSYM_EXPORT(__gtdf2),
    ESP_ELFSYM_EXPORT(__ledf2),
    ESP_ELFSYM_EXPORT(__ltdf2),
    ESP_ELFSYM_EXPORT(__moddi3),
    ESP_ELFSYM_EXPORT(__muldf3),
    ESP_ELFSYM_EXPORT(__muldi3),
    ESP_ELFSYM_EXPORT(__nedf2),
    ESP_ELFSYM_EXPORT(__negdf2),
    ESP_ELFSYM_EXPORT(__powidf2),
    ESP_ELFSYM_EXPORT(__subdf3),
    ESP_ELFSYM_EXPORT(__udivdi3),
    ESP_ELFSYM_EXPORT(__umoddi3),
    ESP_ELFSYM_EXPORT(__unorddf2),

    // ---- 以下为 scripts/gen_symbols.sh 自动生成内容,禁止手改 ----
#include "generated_symbol_table.inc"

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

namespace {

bool DynRemoveRecursive(const std::string& path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    struct dirent* ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        const std::string child = path + "/" + ent->d_name;
        struct stat cst = {};
        if (stat(child.c_str(), &cst) == 0 && S_ISDIR(cst.st_mode)) {
            ok = DynRemoveRecursive(child) && ok;
        } else {
            ok = (unlink(child.c_str()) == 0) && ok;
        }
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0 && ok;
}

}  // namespace

AppInstallResult InstallFromZip(const std::string& zip_path, bool overwrite) {
    (void)overwrite;
    AppInstallResult res;
    if (!SdCardManager::GetInstance().IsMounted()) {
        res.error_msg = "SD 卡未挂载";
        return res;
    }

    reader::ZipReader zip;
    if (!zip.Open(zip_path.c_str())) {
        res.error_msg = "无法打开 ZIP 压缩包或格式损坏: " + zip_path;
        return res;
    }

    std::vector<std::string> entries;
    zip.ListEntries(entries);
    if (entries.empty()) {
        res.error_msg = "ZIP 压缩包为空";
        return res;
    }

    std::string manifest_entry;
    std::string prefix;
    for (const auto& e : entries) {
        if (e == "manifest.json") {
            manifest_entry = e;
            prefix = "";
            break;
        }
        if (e.size() > 14 && e.compare(e.size() - 14, 14, "/manifest.json") == 0) {
            manifest_entry = e;
            prefix = e.substr(0, e.size() - 13);
            break;
        }
    }

    if (manifest_entry.empty()) {
        res.error_msg = "压缩包内未找到 manifest.json";
        return res;
    }

    std::vector<uint8_t> mdata;
    if (!zip.ReadEntry(manifest_entry.c_str(), mdata, 64 * 1024) || mdata.empty()) {
        res.error_msg = "读取 manifest.json 失败";
        return res;
    }

    std::string manifest_str(reinterpret_cast<char*>(mdata.data()), mdata.size());
    cJSON* root = cJSON_Parse(manifest_str.c_str());
    if (!root) {
        res.error_msg = "manifest.json JSON 语法解析错误";
        return res;
    }

    cJSON* j_id = cJSON_GetObjectItem(root, "id");
    cJSON* j_name = cJSON_GetObjectItem(root, "name");
    cJSON* j_ver = cJSON_GetObjectItem(root, "version");
    cJSON* j_abi = cJSON_GetObjectItem(root, "abi");

    if (!cJSON_IsString(j_id) || !j_id->valuestring || strlen(j_id->valuestring) == 0) {
        cJSON_Delete(root);
        res.error_msg = "manifest.json 缺少有效的 id 字段";
        return res;
    }
    if (j_abi && cJSON_IsNumber(j_abi) && static_cast<int>(j_abi->valuedouble) != METALIO_DYNAPP_ABI_VERSION) {
        int got_abi = static_cast<int>(j_abi->valuedouble);
        cJSON_Delete(root);
        res.error_msg = "应用 ABI 版本不匹配 (期望 " + std::to_string(METALIO_DYNAPP_ABI_VERSION) +
                        ", 包为 " + std::to_string(got_abi) + ")";
        return res;
    }

    const std::string app_id = j_id->valuestring;
    const std::string app_name = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : app_id;
    const std::string app_ver = (j_ver && cJSON_IsString(j_ver)) ? j_ver->valuestring : "1.0.0";
    cJSON_Delete(root);

    res.app_id = app_id;
    res.app_name = app_name;
    res.version = app_ver;

    const std::string target_dir = std::string(SD_PATH_APPS) + "/" + app_id + ".app";
    res.bundle_dir = target_dir;
    SdEnsureDir(SD_APP_ROOT);
    SdEnsureDir(SD_PATH_APPS);
    SdEnsureDir(target_dir.c_str());

    constexpr size_t kBufSize = 4096;
    uint8_t* chunk_buf = static_cast<uint8_t*>(heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!chunk_buf) {
        chunk_buf = static_cast<uint8_t*>(malloc(kBufSize));
    }
    if (!chunk_buf) {
        res.error_msg = "内存不足，无法分配解压缓冲区";
        return res;
    }

    for (const auto& e : entries) {
        if (!prefix.empty()) {
            if (e.rfind(prefix, 0) != 0) {
                continue;
            }
        }
        std::string rel = prefix.empty() ? e : e.substr(prefix.size());
        if (rel.empty()) continue;

        if (rel.back() == '/') {
            std::string sub_dir = target_dir + "/" + rel.substr(0, rel.size() - 1);
            SdEnsureDir(sub_dir.c_str());
            continue;
        }

        size_t slash_pos = rel.rfind('/');
        if (slash_pos != std::string::npos) {
            std::string parent_rel = rel.substr(0, slash_pos);
            std::string parent_full = target_dir + "/" + parent_rel;
            for (size_t i = target_dir.size() + 1; i <= parent_full.size(); ++i) {
                if (i == parent_full.size() || parent_full[i] == '/') {
                    SdEnsureDir(parent_full.substr(0, i).c_str());
                }
            }
        }

        std::string out_path = target_dir + "/" + rel;
        if (rel.rfind("data/", 0) == 0) {
            struct stat st = {};
            if (stat(out_path.c_str(), &st) == 0) {
                ESP_LOGI(TAG, "skip existing user data file: %s", out_path.c_str());
                continue;
            }
        }

        if (!zip.OpenEntryStream(e.c_str())) {
            ESP_LOGW(TAG, "failed to open zip entry stream: %s", e.c_str());
            continue;
        }

        std::string tmp_out = out_path + ".unpack.tmp";
        unlink(tmp_out.c_str());
        FILE* fp = fopen(tmp_out.c_str(), "wb");
        if (!fp) {
            zip.CloseEntryStream();
            ESP_LOGE(TAG, "cannot open file for writing: %s", tmp_out.c_str());
            continue;
        }

        bool stream_ok = true;
        while (!zip.EntryStreamEof() && !zip.EntryStreamError()) {
            size_t n = zip.EntryStreamRead(chunk_buf, kBufSize);
            if (n > 0) {
                if (fwrite(chunk_buf, 1, n, fp) != n) {
                    stream_ok = false;
                    break;
                }
            }
        }
        fclose(fp);
        zip.CloseEntryStream();

        if (stream_ok && !zip.EntryStreamError()) {
            unlink(out_path.c_str());
            rename(tmp_out.c_str(), out_path.c_str());
        } else {
            unlink(tmp_out.c_str());
            ESP_LOGE(TAG, "unpack stream failed for %s", e.c_str());
        }
    }

    free(chunk_buf);
    zip.Close();

    SdEnsureDir((target_dir + "/data").c_str());

    // 重新热注册应用
    AppManager::GetInstance().UnregisterApp(app_id.c_str());
    AsyncSettingsStore::GetInstance().SetBool("dynapp", ("in_" + app_id).c_str(), true);
    dynamic_apps::ScanAndRegister();
    HomeScreenRequestAppsRefresh();

    ESP_LOGI(TAG, "app %s (%s) installed/updated successfully to %s",
             app_name.c_str(), app_id.c_str(), target_dir.c_str());
    res.success = true;
    return res;
}

AppInstallResult InstallFromUrl(const std::string& url) {
    AppInstallResult res;
    if (url.empty()) {
        res.error_msg = "URL 不能为空";
        return res;
    }
    SdEnsureDir(SD_APP_ROOT);
    std::string tmp_zip = std::string(SD_APP_ROOT) + "/app_url_download.zip";
    unlink(tmp_zip.c_str());

    std::string err;
    constexpr size_t kMaxZipBytes = 20 * 1024 * 1024; // 20MB
    bool ok = reader::DownloadHttpToFile(url.c_str(), tmp_zip.c_str(), nullptr, 0,
                                         kMaxZipBytes, nullptr, nullptr, err, nullptr);
    if (!ok) {
        unlink(tmp_zip.c_str());
        res.error_msg = "下载应用包失败: " + err;
        return res;
    }

    res = InstallFromZip(tmp_zip, true);
    unlink(tmp_zip.c_str());
    return res;
}

bool UninstallApp(const std::string& app_id, bool keep_data, std::string& out_error) {
    if (app_id.empty()) {
        out_error = "应用 ID 不能为空";
        return false;
    }

    // 1. 从 AppManager 注销
    AppManager::GetInstance().UnregisterApp(app_id.c_str());

    // 2. 清除 NVS 安装标志
    AsyncSettingsStore::GetInstance().SetBool("dynapp", ("in_" + app_id).c_str(), false);

    // 3. 寻找 SD 卡上的应用目录
    std::string bundle_dir = std::string(SD_PATH_APPS) + "/" + app_id + ".app";
    struct stat st = {};
    if (stat(bundle_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        bundle_dir = std::string(SD_PATH_APPS) + "/" + app_id;
        if (stat(bundle_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            bundle_dir.clear();
        }
    }

    if (!bundle_dir.empty()) {
        if (!keep_data) {
            DynRemoveRecursive(bundle_dir);
        } else {
            DIR* dir = opendir(bundle_dir.c_str());
            if (dir) {
                struct dirent* ent;
                while ((ent = readdir(dir)) != nullptr) {
                    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                    if (strcmp(ent->d_name, "data") == 0) continue;
                    std::string child = bundle_dir + "/" + ent->d_name;
                    DynRemoveRecursive(child);
                }
                closedir(dir);
            }
        }
    }

    HomeScreenRequestAppsRefresh();
    ESP_LOGI(TAG, "app %s uninstalled (keep_data=%d)", app_id.c_str(), keep_data ? 1 : 0);
    return true;
}

bool GetAppDetails(const std::string& app_id, AppDetails& out_details) {
    std::string bundle_dir = std::string(SD_PATH_APPS) + "/" + app_id + ".app";
    struct stat st = {};
    if (stat(bundle_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        bundle_dir = std::string(SD_PATH_APPS) + "/" + app_id;
        if (stat(bundle_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            return false;
        }
    }

    out_details.id = app_id;
    out_details.bundle_dir = bundle_dir;
    out_details.installed = (AppManager::GetInstance().GetAppById(app_id.c_str()) != nullptr);
    out_details.enabled = AppManager::GetInstance().IsAppEnabled(app_id.c_str());

    FILE* mf = fopen((bundle_dir + "/manifest.json").c_str(), "rb");
    if (mf) {
        std::string mstr;
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), mf)) > 0 && mstr.size() < 4096) {
            mstr.append(buf, n);
        }
        fclose(mf);
        cJSON* root = cJSON_Parse(mstr.c_str());
        if (root) {
            cJSON* j_name = cJSON_GetObjectItem(root, "name");
            cJSON* j_ver = cJSON_GetObjectItem(root, "version");
            cJSON* j_desc = cJSON_GetObjectItem(root, "description");
            cJSON* j_author = cJSON_GetObjectItem(root, "author");
            if (cJSON_IsString(j_name)) out_details.name = j_name->valuestring;
            if (cJSON_IsString(j_ver)) out_details.version = j_ver->valuestring;
            if (cJSON_IsString(j_desc)) out_details.description = j_desc->valuestring;
            if (cJSON_IsString(j_author)) out_details.author = j_author->valuestring;
            cJSON_Delete(root);
        }
    }
    if (out_details.name.empty()) out_details.name = app_id;

    out_details.has_icon = (stat((bundle_dir + "/icon.png").c_str(), &st) == 0);

    std::string readme_path = bundle_dir + "/README.md";
    if (stat(readme_path.c_str(), &st) == 0) {
        out_details.has_readme = true;
        FILE* rf = fopen(readme_path.c_str(), "rb");
        if (rf) {
            char buf[512];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), rf)) > 0 && out_details.readme_text.size() < 4096) {
                out_details.readme_text.append(buf, n);
            }
            fclose(rf);
        }
    }

    DIR* dir = opendir(bundle_dir.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            std::string cpath = bundle_dir + "/" + ent->d_name;
            struct stat cst = {};
            if (stat(cpath.c_str(), &cst) == 0) {
                if (S_ISREG(cst.st_mode)) {
                    out_details.total_size_bytes += cst.st_size;
                    out_details.file_list.push_back(ent->d_name);
                } else if (S_ISDIR(cst.st_mode)) {
                    out_details.file_list.push_back(std::string(ent->d_name) + "/");
                }
            }
        }
        closedir(dir);
    }
    return true;
}

bool SetAppEnabled(const std::string& app_id, bool enabled) {
    AppManager::GetInstance().SetAppEnabled(app_id.c_str(), enabled);
    HomeScreenRequestAppsRefresh();
    return true;
}

void RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.app.list",
        "List all dynamic applications on the SD card, including their id, name, version, installed status and enabled status.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto pkgs = ListPackages();
            cJSON* arr = cJSON_CreateArray();
            for (const auto& pkg : pkgs) {
                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "id", pkg.id.c_str());
                cJSON_AddStringToObject(obj, "name", pkg.name.c_str());
                cJSON_AddStringToObject(obj, "version", pkg.version.c_str());
                cJSON_AddStringToObject(obj, "dir", pkg.dir.c_str());
                cJSON_AddBoolToObject(obj, "installed", pkg.installed);
                cJSON_AddBoolToObject(obj, "enabled", AppManager::GetInstance().IsAppEnabled(pkg.id.c_str()));
                cJSON_AddItemToArray(arr, obj);
            }
            return arr;
        });

    mcp.AddTool("self.app.get_info",
        "Get detailed information about an installed dynamic application (manifest info, README documentation, files, size). Arg: id.",
        PropertyList({Property("id", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            std::string id = p["id"].value<std::string>();
            AppDetails details;
            cJSON* obj = cJSON_CreateObject();
            if (!GetAppDetails(id, details)) {
                cJSON_AddBoolToObject(obj, "success", false);
                cJSON_AddStringToObject(obj, "error", "未找到指定应用");
                return obj;
            }
            cJSON_AddBoolToObject(obj, "success", true);
            cJSON_AddStringToObject(obj, "id", details.id.c_str());
            cJSON_AddStringToObject(obj, "name", details.name.c_str());
            cJSON_AddStringToObject(obj, "version", details.version.c_str());
            cJSON_AddStringToObject(obj, "description", details.description.c_str());
            cJSON_AddStringToObject(obj, "author", details.author.c_str());
            cJSON_AddStringToObject(obj, "bundle_dir", details.bundle_dir.c_str());
            cJSON_AddBoolToObject(obj, "installed", details.installed);
            cJSON_AddBoolToObject(obj, "enabled", details.enabled);
            cJSON_AddBoolToObject(obj, "has_icon", details.has_icon);
            cJSON_AddBoolToObject(obj, "has_readme", details.has_readme);
            if (details.has_readme) {
                cJSON_AddStringToObject(obj, "readme", details.readme_text.c_str());
            }
            cJSON_AddNumberToObject(obj, "total_size_bytes", details.total_size_bytes);
            cJSON* files_arr = cJSON_CreateArray();
            for (const auto& f : details.file_list) {
                cJSON_AddItemToArray(files_arr, cJSON_CreateString(f.c_str()));
            }
            cJSON_AddItemToObject(obj, "files", files_arr);
            return obj;
        });

    mcp.AddTool("self.app.install",
        "Install or update an external dynamic application from a .zip package on SD card (zip_path) or via remote URL (url). "
        "Binary .app.zip packages can also be posted directly to 'http://<device-ip>:8080/app/install' using curl with PIN. "
        "Args: zip_path (optional path on SD card, e.g. 'radio.app.zip'), url (optional http/https link to download).",
        PropertyList({Property("zip_path", kPropertyTypeString, std::string("")),
                      Property("url", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            std::string zip_path = p["zip_path"].value<std::string>();
            std::string url = p["url"].value<std::string>();

            AppInstallResult res;
            if (!url.empty()) {
                res = InstallFromUrl(url);
            } else if (!zip_path.empty()) {
                std::string full_path = zip_path;
                if (zip_path.rfind("/sdcard", 0) != 0) {
                    if (zip_path[0] == '/') {
                        full_path = std::string(SD_APP_ROOT) + zip_path;
                    } else {
                        full_path = std::string(SD_APP_ROOT) + "/" + zip_path;
                    }
                }
                res = InstallFromZip(full_path, true);
            } else {
                res.error_msg = "必须提供 zip_path 或 url 参数";
            }

            cJSON* obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "success", res.success);
            if (res.success) {
                cJSON_AddStringToObject(obj, "id", res.app_id.c_str());
                cJSON_AddStringToObject(obj, "name", res.app_name.c_str());
                cJSON_AddStringToObject(obj, "version", res.version.c_str());
                cJSON_AddStringToObject(obj, "bundle", res.bundle_dir.c_str());
                cJSON_AddStringToObject(obj, "message", "应用安装/更新成功，已在桌面热注册完毕");
            } else {
                cJSON_AddStringToObject(obj, "error", res.error_msg.c_str());
            }
            return obj;
        });

    mcp.AddTool("self.app.uninstall",
        "Uninstall an external application from the device, removing its bundle files and unregistering from desktop. "
        "Args: id (app ID), keep_data (optional bool, default false to keep data/ folder).",
        PropertyList({Property("id", kPropertyTypeString),
                      Property("keep_data", kPropertyTypeBoolean, false)}),
        [](const PropertyList& p) -> ReturnValue {
            std::string id = p["id"].value<std::string>();
            bool keep_data = p["keep_data"].value<bool>();
            std::string err;
            bool ok = UninstallApp(id, keep_data, err);
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "success", ok);
            cJSON_AddStringToObject(obj, "id", id.c_str());
            if (ok) {
                cJSON_AddStringToObject(obj, "message", "应用已成功卸载");
            } else {
                cJSON_AddStringToObject(obj, "error", err.c_str());
            }
            return obj;
        });

    mcp.AddTool("self.app.enable",
        "Enable an application on the desktop. Arg: id (app ID).",
        PropertyList({Property("id", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            std::string id = p["id"].value<std::string>();
            bool ok = SetAppEnabled(id, true);
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "success", ok);
            cJSON_AddStringToObject(obj, "id", id.c_str());
            cJSON_AddBoolToObject(obj, "enabled", true);
            return obj;
        });

    mcp.AddTool("self.app.disable",
        "Disable an application on the desktop. Arg: id (app ID).",
        PropertyList({Property("id", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            std::string id = p["id"].value<std::string>();
            bool ok = SetAppEnabled(id, false);
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "success", ok);
            cJSON_AddStringToObject(obj, "id", id.c_str());
            cJSON_AddBoolToObject(obj, "enabled", false);
            return obj;
        });

    ESP_LOGI(TAG, "dynamic apps management MCP tools registered");
}

}  // namespace dynamic_apps
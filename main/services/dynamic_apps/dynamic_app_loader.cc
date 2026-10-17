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
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_elf.h>
#include <esp_dlfcn.h>
#include <lvgl.h>

#include "core/app_manager.h"
#include "display/font/fontpack_lvgl.h"
#include "sd_paths.h"

#include "dynamic_app_abi.h"

namespace dynamic_apps {
namespace {

constexpr const char *TAG = "DynApp";
constexpr const char *kManifestName = "manifest.json";
constexpr const char *kSoName = "app.so";

// 供 .so 解析的固件符号表（试点：hello 屏所需 + 大字体桥）。
// 扩展方式：xtensa-esp32s3-elf-nm 新 .so | grep ' U '，把 lv_* 补进来重编固件。
const struct esp_elfsym g_metalio_dynapp_syms[] = {
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
    ESP_ELFSYM_EXPORT(fontpack_lv_font_get),
    ESP_ELFSYM_END
};

using DynAppEntryFn = void (*)(metalio_app_desc_t *);

// 包内路径安全性：拒绝 ".."、绝对路径与反斜杠，防 manifest 图标字段穿越。
bool IconRelPathSafe(const char *rel) {
    if (rel == nullptr || rel[0] == '\0') return false;
    if (rel[0] == '/' || strchr(rel, '\\') != nullptr) return false;
    if (strstr(rel, "..") != nullptr) return false;
    return true;
}

// 加载失败占位屏：绝不把 NULL 喂给 ScreenLoadReplace（NULL 控件 = 无限重绘假死）。
lv_obj_t *MakeFailScreen() {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "App load failed\ncheck serial log");
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    return scr;
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

    memset(out_desc, 0, sizeof(*out_desc));
    entry(out_desc);

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
void OnDynScreenDelete(lv_event_t *e) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(e));
    lv_async_call([](void *p) {
        auto *pth = static_cast<std::string *>(p);
        // dlopen 已加载的 .so 返回同句柄；dlclose 即真实卸载（esp_elf_deinit 释放代码段）。
        void *h = dlopen(pth->c_str(), RTLD_NOW);
        if (h != nullptr) {
            dlclose(h);
            ESP_LOGI(TAG, "dynamic app unloaded: %s", pth->c_str());
        }
        delete pth;
    }, path);
}

// 惰性动态应用模块：持有 manifest 元数据与 app.so 路径；点开时 dlopen 创建屏幕，
// 屏幕销毁后 dlclose 卸载。无静态工厂（需实例状态），走 CreateUI 路径。
class LazyDynamicAppModule : public AppModule {
public:
    LazyDynamicAppModule(std::string id, std::string name, std::string icon, std::string so_path)
        : id_(std::move(id)), name_(std::move(name)), icon_(std::move(icon)), so_path_(std::move(so_path)) {}

    const char *GetId() const override { return id_.c_str(); }
    const char *GetName() const override { return name_.c_str(); }
    const char *GetIconPath() const override { return icon_.c_str(); }
    bool IsMandatory() const override { return false; }
    lv_obj_t *CreateUI(lv_obj_t * /*parent*/ = nullptr) override {
        void *handle = nullptr;
        metalio_app_desc_t desc = {};
        if (!ReadDesc(so_path_.c_str(), &handle, &desc)) {
            return MakeFailScreen();
        }
        lv_obj_t *scr = static_cast<lv_obj_t *>(desc.create());
        if (scr == nullptr) {
            ESP_LOGE(TAG, "%s: create returned null", so_path_.c_str());
            dlclose(handle);
            return MakeFailScreen();
        }
        // 卸载钩子交给屏幕：树删除完成后 async dlclose
        lv_obj_add_event_cb(scr, OnDynScreenDelete, LV_EVENT_DELETE, new std::string(so_path_));
        ESP_LOGI(TAG, "dynamic app loaded: %s (%s)", name_.c_str(), so_path_.c_str());
        return scr;
    }
    ScreenFactory GetScreenFactory() const override { return nullptr; }

private:
    std::string id_;
    std::string name_;
    std::string icon_;
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

// 扫描单个应用目录：读 manifest（无代码执行）→ 校验 → 注册惰性模块。
void RegisterFromDir(const std::string &dir_path) {
    const std::string manifest_path = dir_path + "/" + kManifestName;
    const std::string text = ReadFileText(manifest_path.c_str());
    if (text.empty()) {
        ESP_LOGD(TAG, "%s: no manifest, skip", manifest_path.c_str());
        return;
    }

    cJSON *root = cJSON_Parse(text.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "%s: manifest parse failed", manifest_path.c_str());
        return;
    }

    std::string id, name, icon_rel;
    int abi = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (item->string == nullptr) continue;
        if (strcmp(item->string, "id") == 0 && cJSON_IsString(item)) {
            id = item->valuestring;
        } else if (strcmp(item->string, "name") == 0 && cJSON_IsString(item)) {
            name = item->valuestring;
        } else if (strcmp(item->string, "icon") == 0 && cJSON_IsString(item)) {
            icon_rel = item->valuestring;
        } else if (strcmp(item->string, "abi") == 0 && cJSON_IsNumber(item)) {
            abi = static_cast<int>(item->valuedouble);
        }
    }
    cJSON_Delete(root);

    if (abi != METALIO_DYNAPP_ABI_VERSION) {
        // 扫描阶段闸门：abi 不匹配的包点名拒绝（固件升级后旧包在此失效，不执行代码）
        ESP_LOGE(TAG, "%s: abi %d != %u, skip", manifest_path.c_str(), abi, METALIO_DYNAPP_ABI_VERSION);
        return;
    }
    if (id.empty() || id.size() > 12) {
        // NVS 开关键为 "en_<id>"，总长 ≤15
        ESP_LOGE(TAG, "%s: id '%s' invalid (need 1..12 chars)", manifest_path.c_str(), id.c_str());
        return;
    }
    if (name.empty()) {
        ESP_LOGE(TAG, "%s: name empty", manifest_path.c_str());
        return;
    }

    const std::string so_path = dir_path + "/" + kSoName;
    struct stat st = {};
    if (stat(so_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "%s: %s missing, skip", manifest_path.c_str(), kSoName);
        return;
    }

    // 图标：S: 盘符（LVGL POSIX FS → /sdcard）+ 卡内相对路径；缺省用内置图标
    std::string icon = "A:ic_app_home_clock.spng";
    if (IconRelPathSafe(icon_rel.c_str())) {
        icon = std::string("S:/") + SdUserPath(dir_path.c_str()) + "/" + icon_rel;
    } else if (!icon_rel.empty()) {
        ESP_LOGW(TAG, "%s: unsafe icon path '%s', using builtin", manifest_path.c_str(), icon_rel.c_str());
    }

    if (AppManager::GetInstance().GetAppById(id.c_str()) != nullptr) {
        // 动态应用先于内置应用注册，同名时内置后注册会替换动态入口（内置优先）
        ESP_LOGI(TAG, "dynamic app %s already registered, skip", id.c_str());
        return;
    }

    auto module = std::make_shared<LazyDynamicAppModule>(
        std::move(id), std::move(name), std::move(icon), so_path);
    AppManager::GetInstance().RegisterApp(module);
    ESP_LOGI(TAG, "dynamic app available: %s (%s) — lazy, loads on tap",
             module->GetName(), module->GetId());
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

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        const std::string dir_path = std::string(SD_PATH_APPS) + "/" + ent->d_name;
        struct stat st = {};
        if (stat(dir_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        RegisterFromDir(dir_path);
        ++found;
    }
    closedir(dir);

    if (found > 0) {
        ESP_LOGI(TAG, "dynamic app scan done: %d dirs under %s", found, SD_PATH_APPS);
    }
}

}  // namespace dynamic_apps

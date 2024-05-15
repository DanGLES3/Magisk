#include <android/dlext.h>
#include <sys/mount.h>
#include <dlfcn.h>
#include <regex.h>
#include <unwind.h>
#include <bitset>
#include <list>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <lsplt.hpp>

#include <base.hpp>
#include <consts.hpp>

#include "zygisk.hpp"
#include "memory.hpp"
#include "module.hpp"
#include "solist.hpp"

using namespace std;
using jni_hook::hash_map;
using jni_hook::tree_map;
using xstring = jni_hook::string;

// Extreme verbose logging
#define ZLOGV(...) ZLOGD(__VA_ARGS__)
//#define ZLOGV(...) (void*)0

static void hook_unloader();
static void unhook_functions();
static void hook_jni_env();
static void restore_jni_env(JNIEnv *env);

namespace {

enum {
    POST_SPECIALIZE,
    APP_FORK_AND_SPECIALIZE,
    APP_SPECIALIZE,
    SERVER_FORK_AND_SPECIALIZE,
    DO_REVERT_UNMOUNT,
    SKIP_CLOSE_LOG_PIPE,
    DO_ALLOW,
    ALLOWLIST_ENFORCED,
    RESTORE_MOUNT_EXTERNAL_NONE,
    DO_FUTILE_HIDE,

    FLAG_MAX
};

#define MAX_FD_SIZE 1024

// Global variables
vector<tuple<dev_t, ino_t, const char *, void **>> *plt_hook_list;
map<string, vector<JNINativeMethod>, StringCmp> *jni_hook_list;
hash_map<xstring, tree_map<xstring, tree_map<xstring, void *>>> *jni_method_map;
bool should_unmap_zygisk = false;

// Current context
HookContext *g_ctx;
const JNINativeInterface *old_functions = nullptr;
JNINativeInterface *new_functions = nullptr;

#define DCL_PRE_POST(name) \
void name##_pre();         \
void name##_post();

struct HookContext {
    JNIEnv *env;
    union {
        void *ptr;
        AppSpecializeArgs_v5 *app;
        ServerSpecializeArgs_v1 *server;
    } args;

    const char *process;
    list<ZygiskModule> modules;

    int pid;
    bitset<FLAG_MAX> flags;
    uint32_t info_flags;
    bitset<MAX_FD_SIZE> allowed_fds;
    vector<int> exempted_fds;

    struct RegisterInfo {
        regex_t regex;
        string symbol;
        void *callback;
        void **backup;
    };

    struct IgnoreInfo {
        regex_t regex;
        string symbol;
    };

    pthread_mutex_t hook_info_lock;
    vector<RegisterInfo> register_info;
    vector<IgnoreInfo> ignore_info;

    HookContext(JNIEnv *env, void *args) :
    env(env), args{args}, process(nullptr), pid(-1), info_flags(0),
    hook_info_lock(PTHREAD_MUTEX_INITIALIZER) {
        static bool restored_env = false;
        if (!restored_env) {
            restore_jni_env(env);
            restored_env = true;
        }
        g_ctx = this;
    }

    ~HookContext();

    void run_modules_pre(const vector<int> &fds);
    void run_modules_post();
    DCL_PRE_POST(fork)
    DCL_PRE_POST(app_specialize)
    DCL_PRE_POST(server_specialize)
    DCL_PRE_POST(nativeForkAndSpecialize)
    DCL_PRE_POST(nativeSpecializeAppProcess)
    DCL_PRE_POST(nativeForkSystemServer)

    void sanitize_fds();
    bool exempt_fd(int fd);
    bool is_child() const { return pid <= 0; }
    bool can_exempt_fd() const { return flags[APP_FORK_AND_SPECIALIZE] && args.app->fds_to_ignore; }

    // Compatibility shim
    void plt_hook_register(const char *regex, const char *symbol, void *fn, void **backup);
    void plt_hook_exclude(const char *regex, const char *symbol);
    void plt_hook_process_regex();

    bool plt_hook_commit();
};

#undef DCL_PRE_POST

// -----------------------------------------------------------------

#define DCL_HOOK_FUNC(ret, func, ...) \
ret (*old_##func)(__VA_ARGS__);       \
ret new_##func(__VA_ARGS__)

DCL_HOOK_FUNC(void, androidSetCreateThreadFunc, void *func) {
    ZLOGD("androidSetCreateThreadFunc\n");
    hook_jni_env();
    old_androidSetCreateThreadFunc(func);
}

// Skip actual fork and return cached result if applicable
DCL_HOOK_FUNC(int, fork) {
    return (g_ctx && g_ctx->pid >= 0) ? g_ctx->pid : old_fork();
}

// Unmount stuffs in the process's private mount namespace
DCL_HOOK_FUNC(int, unshare, int flags) {
    int res = old_unshare(flags);
    if (g_ctx && (flags & CLONE_NEWNS) != 0 && res == 0) {
        int ret = (g_ctx->flags[DO_ALLOW])?
                remote_request_sulist() :
        (g_ctx->flags[ALLOWLIST_ENFORCED] == false && g_ctx->flags[DO_REVERT_UNMOUNT])?
                remote_request_umount() : 0 /* do nothing */;
        if (ret == -1) ZLOGE("remote request failed\n");
        // clean up mount id hole by unshare mount namespace twice
        old_unshare(CLONE_NEWNS);
        // restore MOUNT_EXTERNAL_NONE
        if (g_ctx->flags[RESTORE_MOUNT_EXTERNAL_NONE])
            g_ctx->args.app->mount_external = 0;
        logging_muted = true;
        // Restore errno back to 0
        errno = 0;
    }
    return res;
}

// This is the last moment before the secontext of the process changes
DCL_HOOK_FUNC(int, selinux_android_setcontext,
              uid_t uid, bool isSystemServer, const char *seinfo, const char *pkgname) {
    // Pre-fetch logd before secontext transition
    zygisk_get_logd();
    return old_selinux_android_setcontext(uid, isSystemServer, seinfo, pkgname);
}

// Close file descriptors to prevent crashing
DCL_HOOK_FUNC(void, android_log_close) {
    if (g_ctx == nullptr || !g_ctx->flags[SKIP_CLOSE_LOG_PIPE]) {
        // This happens during forks like nativeForkApp, nativeForkUsap,
        // nativeForkSystemServer, and nativeForkAndSpecialize.
        zygisk_close_logd();
    }
    old_android_log_close();
}

// We cannot directly call `dlclose` to unload ourselves, otherwise when `dlclose` returns,
// it will return to our code which has been unmapped, causing segmentation fault.
// Instead, we hook `pthread_attr_destroy` which will be called when VM daemon threads start.
DCL_HOOK_FUNC(int, pthread_attr_destroy, void *target) {
    int res = old_pthread_attr_destroy((pthread_attr_t *)target);

    // Only perform unloading on the main thread
    if (gettid() != getpid())
        return res;

    ZLOGV("pthread_attr_destroy\n");
    if (should_unmap_zygisk) {
        unhook_functions();
        if (should_unmap_zygisk) {
            // Because both `pthread_attr_destroy` and `dlclose` have the same function signature,
            // we can use `musttail` to let the compiler reuse our stack frame and thus
            // `dlclose` will directly return to the caller of `pthread_attr_destroy`.
            [[clang::musttail]] return dlclose(self_handle);
        }
    }

    return res;
}

#undef DCL_HOOK_FUNC

// -----------------------------------------------------------------

void hookJniNativeMethods(JNIEnv *env, const char *clz, JNINativeMethod *methods, int numMethods) {
    auto class_map = jni_method_map->find(clz);
    if (class_map == jni_method_map->end()) {
        for (int i = 0; i < numMethods; ++i) {
            methods[i].fnPtr = nullptr;
        }
        return;
    }

    vector<JNINativeMethod> hooks;
    for (int i = 0; i < numMethods; ++i) {
        auto method_map = class_map->second.find(methods[i].name);
        if (method_map != class_map->second.end()) {
            auto it = method_map->second.find(methods[i].signature);
            if (it != method_map->second.end()) {
                // Copy the JNINativeMethod
                hooks.push_back(methods[i]);
                // Save the original function pointer
                methods[i].fnPtr = it->second;
                // Do not allow double hook, remove method from map
                method_map->second.erase(it);
                continue;
            }
        }
        // No matching method found, set fnPtr to null
        methods[i].fnPtr = nullptr;
    }

    if (hooks.empty())
        return;

    old_functions->RegisterNatives(
            env, env->FindClass(clz), hooks.data(), static_cast<int>(hooks.size()));
}

ZygiskModule::ZygiskModule(int id, void *handle, void *entry)
: id(id), handle(handle), entry{entry}, api{}, mod{nullptr} {
    // Make sure all pointers are null
    memset(&api, 0, sizeof(api));
    api.base.impl = this;
    api.base.registerModule = &ZygiskModule::RegisterModuleImpl;
}

bool ZygiskModule::RegisterModuleImpl(ApiTable *api, long *module) {
    if (api == nullptr || module == nullptr)
        return false;

    long api_version = *module;
    // Unsupported version
    if (api_version > ZYGISK_API_VERSION)
        return false;

    // Set the actual module_abi*
    api->base.impl->mod = { module };

    // Fill in API accordingly with module API version
    if (api_version >= 1) {
        api->v1.hookJniNativeMethods = hookJniNativeMethods;
        api->v1.pltHookRegister = [](auto a, auto b, auto c, auto d) {
            if (g_ctx) g_ctx->plt_hook_register(a, b, c, d);
        };
        api->v1.pltHookExclude = [](auto a, auto b) {
            if (g_ctx) g_ctx->plt_hook_exclude(a, b);
        };
        api->v1.pltHookCommit = []() { return g_ctx && g_ctx->plt_hook_commit(); };
        api->v1.connectCompanion = [](ZygiskModule *m) { return m->connectCompanion(); };
        api->v1.setOption = [](ZygiskModule *m, auto opt) { m->setOption(opt); };
    }
    if (api_version >= 2) {
        api->v2.getModuleDir = [](ZygiskModule *m) { return m->getModuleDir(); };
        api->v2.getFlags = [](auto) { return ZygiskModule::getFlags(); };
    }
    if (api_version >= 4) {
        api->v4.pltHookCommit = lsplt::CommitHook;
        api->v4.pltHookRegister = [](dev_t dev, ino_t inode, const char *symbol, void *fn, void **backup) {
            if (dev == 0 || inode == 0 || symbol == nullptr || fn == nullptr)
                return;
            lsplt::RegisterHook(dev, inode, symbol, fn, backup);
        };
        api->v4.exemptFd = [](int fd) { return g_ctx && g_ctx->exempt_fd(fd); };
    }

    return true;
}

void HookContext::plt_hook_register(const char *regex, const char *symbol, void *fn, void **backup) {
    if (regex == nullptr || symbol == nullptr || fn == nullptr)
        return;
    regex_t re;
    if (regcomp(&re, regex, REG_NOSUB) != 0)
        return;
    mutex_guard lock(hook_info_lock);
    register_info.emplace_back(RegisterInfo{re, symbol, fn, backup});
}

void HookContext::plt_hook_exclude(const char *regex, const char *symbol) {
    if (!regex) return;
    regex_t re;
    if (regcomp(&re, regex, REG_NOSUB) != 0)
        return;
    mutex_guard lock(hook_info_lock);
    ignore_info.emplace_back(IgnoreInfo{re, symbol ?: ""});
}

void HookContext::plt_hook_process_regex() {
    if (register_info.empty())
        return;
    for (auto &map : lsplt::MapInfo::Scan()) {
        if (map.offset != 0 || !map.is_private || !(map.perms & PROT_READ)) continue;
        for (auto &reg: register_info) {
            if (regexec(&reg.regex, map.path.data(), 0, nullptr, 0) != 0)
                continue;
            bool ignored = false;
            for (auto &ign: ignore_info) {
                if (regexec(&ign.regex, map.path.data(), 0, nullptr, 0) != 0)
                    continue;
                if (ign.symbol.empty() || ign.symbol == reg.symbol) {
                    ignored = true;
                    break;
                }
            }
            if (!ignored) {
                lsplt::RegisterHook(map.dev, map.inode, reg.symbol, reg.callback, reg.backup);
            }
        }
    }
}

bool HookContext::plt_hook_commit() {
    {
        mutex_guard lock(hook_info_lock);
        plt_hook_process_regex();
        register_info.clear();
        ignore_info.clear();
    }
    return lsplt::CommitHook();
}

bool ZygiskModule::valid() const {
    if (mod.api_version == nullptr)
        return false;
    switch (*mod.api_version) {
    case 4:
    case 3:
    case 2:
    case 1:
        return mod.v1->impl && mod.v1->preAppSpecialize && mod.v1->postAppSpecialize &&
            mod.v1->preServerSpecialize && mod.v1->postServerSpecialize;
    default:
        return false;
    }
}

int ZygiskModule::connectCompanion() const {
    if (int fd = zygisk_request(ZygiskRequest::CONNECT_COMPANION); fd >= 0) {
        write_int(fd, id);
        return fd;
    }
    return -1;
}

int ZygiskModule::getModuleDir() const {
    if (int fd = zygisk_request(ZygiskRequest::GET_MODDIR); fd >= 0) {
        write_int(fd, id);
        int dfd = recv_fd(fd);
        close(fd);
        return dfd;
    }
    return -1;
}

void ZygiskModule::setOption(zygisk::Option opt) {
    if (g_ctx == nullptr)
        return;
    switch (opt) {
    case zygisk::FORCE_DENYLIST_UNMOUNT:
        g_ctx->flags[DO_REVERT_UNMOUNT] = true;
        break;
    case zygisk::DLCLOSE_MODULE_LIBRARY:
        unload = true;
        break;
    }
}

uint32_t ZygiskModule::getFlags() {
    return g_ctx ? (g_ctx->info_flags & ~PRIVATE_MASK) : 0;
}

// -----------------------------------------------------------------

int sigmask(int how, int signum) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signum);
    return sigprocmask(how, &set, nullptr);
}

void HookContext::fork_pre() {
    // Do our own fork before loading any 3rd party code
    // First block SIGCHLD, unblock after original fork is done
    sigmask(SIG_BLOCK, SIGCHLD);
    pid = old_fork();

    if (!is_child())
        return;

    // Record all open fds
    auto dir = xopen_dir("/proc/self/fd");
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        int fd = parse_int(entry->d_name);
        if (fd < 0 || fd >= MAX_FD_SIZE) {
            close(fd);
            continue;
        }
        allowed_fds[fd] = true;
    }
    // The dirfd will be closed once out of scope
    allowed_fds[dirfd(dir.get())] = false;
    // logd_fd should be handled separately
    if (int fd = zygisk_get_logd(); fd >= 0) {
        allowed_fds[fd] = false;
    }
}

void HookContext::fork_post() {
    // Unblock SIGCHLD in case the original method didn't
    sigmask(SIG_UNBLOCK, SIGCHLD);
}

void HookContext::sanitize_fds() {
    zygisk_close_logd();

    if (!is_child()) {
        return;
    }

    if (can_exempt_fd() && !exempted_fds.empty()) {
        auto update_fd_array = [&](int old_len) -> jintArray {
            jintArray array = env->NewIntArray(static_cast<int>(old_len + exempted_fds.size()));
            if (array == nullptr)
                return nullptr;

            env->SetIntArrayRegion(
                    array, old_len, static_cast<int>(exempted_fds.size()), exempted_fds.data());
            for (int fd : exempted_fds) {
                if (fd >= 0 && fd < MAX_FD_SIZE) {
                    allowed_fds[fd] = true;
                }
            }
            *args.app->fds_to_ignore = array;
            return array;
        };

        if (jintArray fdsToIgnore = *args.app->fds_to_ignore) {
            int *arr = env->GetIntArrayElements(fdsToIgnore, nullptr);
            int len = env->GetArrayLength(fdsToIgnore);
            for (int i = 0; i < len; ++i) {
                int fd = arr[i];
                if (fd >= 0 && fd < MAX_FD_SIZE) {
                    allowed_fds[fd] = true;
                }
            }
            if (jintArray newFdList = update_fd_array(len)) {
                env->SetIntArrayRegion(newFdList, 0, len, arr);
            }
            env->ReleaseIntArrayElements(fdsToIgnore, arr, JNI_ABORT);
        } else {
            update_fd_array(0);
        }
    }

    // Close all forbidden fds to prevent crashing
    auto dir = xopen_dir("/proc/self/fd");
    int dfd = dirfd(dir.get());
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        int fd = parse_int(entry->d_name);
        if ((fd < 0 || fd >= MAX_FD_SIZE || !allowed_fds[fd]) && fd != dfd) {
            close(fd);
        }
    }
}

void HookContext::run_modules_pre(const vector<int> &fds) {
    for (int i = 0; i < fds.size(); ++i) {
        struct stat s{};
        if (fstat(fds[i], &s) != 0 || !S_ISREG(s.st_mode)) {
            close(fds[i]);
            continue;
        }
        android_dlextinfo info {
            .flags = ANDROID_DLEXT_USE_LIBRARY_FD,
            .library_fd = fds[i],
        };
        if (void *h = android_dlopen_ext("/jit-zygisk-cache", RTLD_LAZY, &info)) {
            if (void *e = dlsym(h, "zygisk_module_entry")) {
                modules.emplace_back(i, h, e);
            }
        } else if (g_ctx->flags[SERVER_FORK_AND_SPECIALIZE]) {
            ZLOGW("Failed to dlopen zygisk module: %s\n", dlerror());
        }
        close(fds[i]);
    }

    if (flags[DO_FUTILE_HIDE]) {
        // Remove from maps to avoid detection
        for (auto &info : lsplt::MapInfo::Scan()) {
            if (strstr(info.path.data(), "/memfd:jit-zygisk-cache") == nullptr &&
                strstr(info.path.data(), "/modules/") == nullptr)
                continue;
            ZLOGD("hide_remap: addr=[%p-%p]\n", 
                info.start, info.end, info.dev, info.inode, info.path.data());
            void *addr = reinterpret_cast<void *>(info.start);
            size_t size = info.end - info.start;
            void *copy = xmmap(nullptr, size, PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
            if ((info.perms & PROT_READ) == 0) {
                mprotect(addr, size, PROT_READ);
            }
            memcpy(copy, addr, size);
            mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, addr);
            mprotect(addr, size, info.perms);
        }
    }

    for (auto it = modules.begin(); it != modules.end();) {
        it->onLoad(env);
        if (it->valid()) {
            ++it;
        } else {
            it = modules.erase(it);
        }
    }

    for (auto &m : modules) {
        if (flags[APP_SPECIALIZE]) {
            m.preAppSpecialize(args.app);
        } else if (flags[SERVER_FORK_AND_SPECIALIZE]) {
            m.preServerSpecialize(args.server);
        }
    }
}

void HookContext::run_modules_post() {
    flags[POST_SPECIALIZE] = true;
    for (const auto &m : modules) {
        if (flags[APP_SPECIALIZE]) {
            m.postAppSpecialize(args.app);
        } else if (flags[SERVER_FORK_AND_SPECIALIZE]) {
            m.postServerSpecialize(args.server);
        }
        m.tryUnload();
    }
    if (flags[DO_FUTILE_HIDE] && SoList::Initialize()) {
        SoList::NullifySoName("/memfd:jit-zygisk-cache");
        SoList::NullifySoName("/modules/");
    }
        // Remap as well to avoid checking of /memfd:jit-cache
    for (auto &info : lsplt::MapInfo::Scan()) {
        if (strstr(info.path.c_str(), "jit-cache-zygisk"))
        {
            void *addr = (void *)info.start;
            size_t size = info.end - info.start;
            // MAP_SHARED should fix the suspicious mapping.
            void *copy = mmap(nullptr, size, PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
            if (copy == MAP_FAILED) {
                LOGE("Failed to mmap jit-cache-zygisk\n");
                continue;
            }

            if ((info.perms & PROT_READ) == 0) {
                mprotect(addr, size, PROT_READ);
            }
            memcpy(copy, addr, size);
            mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, addr);
            mprotect(addr, size, info.perms);
        }
    }

    // Don't know if there's a header for things like this
    // so I just put it into a lambda
    auto generateRandomString = [](char *str, int length) {
        const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        srand(time(NULL));

        for (int i = 0; i < length; i++) {
            int key = rand() % (sizeof(charset) - 1);
            str[i] = charset[key];
        }

        str[length] = '\0';
    };

    // Randomize name of anonymous mappings
    // We don't run this in the previous loop because LSPosed might also add
    // mappings that are not related to /memfd:jit-zygisk-cache
    //
    // Since we changed to MAP_SHARED, I don't think this is still needed but let's
    // leave it here just in case.
    for (auto info : lsplt::MapInfo::Scan()) {
        // I had some problems with info.perms & PROT_EXEC so I had to change lsplt source a bit.
        // If that problem occurs here, do strchr(info.perms_str.c_str(), 'x') instead and add perms_str
        // to the lsplt MapInfo struct and set it to the raw perms string in Scan();
        if (info.perms & PROT_EXEC && info.path.empty()) {
            // Generate Random Name
            char randomString[11];
            generateRandomString(randomString, 10);
            LOGI("Randomized Memory map name: %s", randomString);

            // Memory address of random string
            uintptr_t strAddr = (uintptr_t)&randomString;

            // https://lore.kernel.org/lkml/1383170047-21074-2-git-send-email-ccross@android.com/
            prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, info.start, info.end - info.start, strAddr);
        }
        // Remap as MAP_SHARED
        if (info.perms & PROT_EXEC && info.dev == 0 && info.path.find("anon") != std::string::npos) {
            void *addr = reinterpret_cast<void *>(info.start);
            size_t size = info.end - info.start;
            void *copy = mmap(nullptr, size, PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
            if ((info.perms & PROT_READ) == 0) {
                mprotect(addr, size, PROT_READ);
            }
            memcpy(copy, addr, size);
            mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, addr);
            mprotect(addr, size, info.perms);
        }
    }
}


void HookContext::app_specialize_pre() {
    flags[APP_SPECIALIZE] = true;

    vector<int> module_fds;
    int fd = remote_get_info(args.app->uid, process, &info_flags, module_fds);
    if ((info_flags & ALLOWLIST_ENFORCING) == ALLOWLIST_ENFORCING) {
        flags[ALLOWLIST_ENFORCED] = true;
        if ((info_flags & PROCESS_ON_ALLOWLIST) == PROCESS_ON_ALLOWLIST ||
            (info_flags & PROCESS_IS_MAGISK_APP) == PROCESS_IS_MAGISK_APP) {
            ZLOGI("[%s] is on the allowlist\n", process);
            flags[DO_ALLOW] = true;
            // Ensure separated namespace, allow denylist to handle isolated process before Android 11
            if (args.app->mount_external == 0 /* MOUNT_EXTERNAL_NONE */) {
                args.app->mount_external = 1 /* MOUNT_EXTERNAL_DEFAULT */;
                flags[RESTORE_MOUNT_EXTERNAL_NONE] = true;
            }
        } else {
            flags[DO_FUTILE_HIDE] = (args.app->uid % 100000) >= 10000;
        }
    } else if ((info_flags & UNMOUNT_MASK) == UNMOUNT_MASK) {
        ZLOGI("[%s] is on the hidelist\n", process);
        flags[DO_REVERT_UNMOUNT] = true;
        flags[DO_FUTILE_HIDE] = (args.app->uid % 100000) >= 10000;
        // Ensure separated namespace, allow denylist to handle isolated process before Android 11
        if (args.app->mount_external == 0 /* MOUNT_EXTERNAL_NONE */) {
            // Only apply the fix before Android 11, as it can cause undefined behaviour in later versions
            char sdk_ver_str[92]; // PROPERTY_VALUE_MAX
            if (__system_property_get("ro.build.version.sdk", sdk_ver_str) && atoi(sdk_ver_str) < 30) {
                args.app->mount_external = 1 /* MOUNT_EXTERNAL_DEFAULT */;
                flags[RESTORE_MOUNT_EXTERNAL_NONE] = true;
            }
        }
    }

    if (fd >= 0) {
        run_modules_pre(module_fds);
    }
    close(fd);
}

void HookContext::app_specialize_post() {
    run_modules_post();
    if (info_flags & PROCESS_IS_MAGISK_APP) {
        setenv("ZYGISK_ENABLED", "1", 1);
    }

    // Cleanups
    env->ReleaseStringUTFChars(args.app->nice_name, process);
}

void HookContext::server_specialize_pre() {
    vector<int> module_fds;
    int fd = remote_get_info(1000, "system_server", &info_flags, module_fds);
    if (fd >= 0) {
        if (module_fds.empty()) {
            write_int(fd, 0);
        } else {
            run_modules_pre(module_fds);

            // Send the bitset of module status back to magiskd from system_server
            dynamic_bitset bits;
            for (const auto &m : modules)
                bits[m.getId()] = true;
            write_int(fd, static_cast<int>(bits.slots()));
            for (int i = 0; i < bits.slots(); ++i) {
                auto l = bits.get_slot(i);
                xwrite(fd, &l, sizeof(l));
            }
        }
        close(fd);
    }
}

void HookContext::server_specialize_post() {
    run_modules_post();
}

HookContext::~HookContext() {
    // This global pointer points to a variable on the stack.
    // Set this to nullptr to prevent leaking local variable.
    // This also disables most plt hooked functions.
    g_ctx = nullptr;

    if (!is_child())
        return;

    zygisk_close_logd();
    android_logging();

    should_unmap_zygisk = true;

    // Unhook JNI methods
    for (const auto &[clz, methods] : *jni_hook_list) {
        if (!methods.empty() && env->RegisterNatives(
                env->FindClass(clz.data()), methods.data(),
                static_cast<int>(methods.size())) != 0) {
            ZLOGE("Failed to restore JNI hook of class [%s]\n", clz.data());
            should_unmap_zygisk = false;
        }
    }
    delete jni_hook_list;
    jni_hook_list = nullptr;

    // Do NOT directly call delete
    operator delete(jni_method_map);
    // Directly unmap the whole memory block
    jni_hook::memory_block::release();
    jni_method_map = nullptr;

    // Strip out all API function pointers
    for (auto &m : modules) {
        m.clearApi();
    }

    hook_unloader();
}

bool HookContext::exempt_fd(int fd) {
    if (flags[POST_SPECIALIZE] || flags[SKIP_CLOSE_LOG_PIPE])
        return true;
    if (!can_exempt_fd())
        return false;
    exempted_fds.push_back(fd);
    return true;
}

// -----------------------------------------------------------------

void HookContext::nativeSpecializeAppProcess_pre() {
    process = env->GetStringUTFChars(args.app->nice_name, nullptr);
    ZLOGV("pre  specialize [%s]\n", process);
    // App specialize does not check FD
    flags[SKIP_CLOSE_LOG_PIPE] = true;
    app_specialize_pre();
}

void HookContext::nativeSpecializeAppProcess_post() {
    ZLOGV("post specialize [%s]\n", process);
    app_specialize_post();
}

void HookContext::nativeForkSystemServer_pre() {
    ZLOGV("pre  forkSystemServer\n");
    flags[SERVER_FORK_AND_SPECIALIZE] = true;

    fork_pre();
    if (is_child()) {
        server_specialize_pre();
    }
    sanitize_fds();
}

void HookContext::nativeForkSystemServer_post() {
    if (is_child()) {
        ZLOGV("post forkSystemServer\n");
        server_specialize_post();
    }
    fork_post();
}

void HookContext::nativeForkAndSpecialize_pre() {
    process = env->GetStringUTFChars(args.app->nice_name, nullptr);
    ZLOGV("pre  forkAndSpecialize [%s]\n", process);
    flags[APP_FORK_AND_SPECIALIZE] = true;

    fork_pre();
    if (is_child()) {
        app_specialize_pre();
    }
    sanitize_fds();
}

void HookContext::nativeForkAndSpecialize_post() {
    if (is_child()) {
        ZLOGV("post forkAndSpecialize [%s]\n", process);
        app_specialize_post();
    }
    fork_post();
}

} // namespace

// -----------------------------------------------------------------


static void hook_register(dev_t dev, ino_t inode, const char *symbol, void *new_func, void **old_func) {
    if (!lsplt::RegisterHook(dev, inode, symbol, new_func, old_func)) {
        ZLOGE("Failed to register plt_hook \"%s\"\n", symbol);
        return;
    }
    plt_hook_list->emplace_back(dev, inode, symbol, old_func);
}

static void hook_commit() {
    if (!lsplt::CommitHook())
        ZLOGE("plt_hook failed\n");
}

#define PLT_HOOK_REGISTER_SYM(DEV, INODE, SYM, NAME) \
    hook_register(DEV, INODE, SYM, \
    reinterpret_cast<void *>(new_##NAME), reinterpret_cast<void **>(&old_##NAME))

#define PLT_HOOK_REGISTER(DEV, INODE, NAME) \
    PLT_HOOK_REGISTER_SYM(DEV, INODE, #NAME, NAME)

void hook_functions() {
    default_new(plt_hook_list);
    default_new(jni_hook_list);
    default_new(jni_method_map);

    ino_t android_runtime_inode = 0;
    dev_t android_runtime_dev = 0;

    for (auto &map : lsplt::MapInfo::Scan()) {
        if (map.path.ends_with("/libandroid_runtime.so")) {
            android_runtime_inode = map.inode;
            android_runtime_dev = map.dev;
        }
    }

    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, fork);
    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, unshare);
    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, androidSetCreateThreadFunc);
    PLT_HOOK_REGISTER(android_runtime_dev, android_runtime_inode, selinux_android_setcontext);
    PLT_HOOK_REGISTER_SYM(android_runtime_dev, android_runtime_inode, "__android_log_close", android_log_close);
    hook_commit();

    // Remove unhooked methods
    plt_hook_list->erase(
            std::remove_if(plt_hook_list->begin(), plt_hook_list->end(),
            [](auto &t) { return *std::get<3>(t) == nullptr;}),
            plt_hook_list->end());
}

static void hook_unloader() {
    ino_t art_inode = 0;
    dev_t art_dev = 0;

    for (auto &map : lsplt::MapInfo::Scan()) {
        if (map.path.ends_with("/libart.so")) {
            art_inode = map.inode;
            art_dev = map.dev;
            break;
        }
    }

    PLT_HOOK_REGISTER(art_dev, art_inode, pthread_attr_destroy);
    hook_commit();
}

static void unhook_functions() {
    // Unhook plt_hook
    for (const auto &[dev, inode, sym, old_func] : *plt_hook_list) {
        if (!lsplt::RegisterHook(dev, inode, sym, *old_func, nullptr)) {
            ZLOGE("Failed to register plt_hook [%s]\n", sym);
            should_unmap_zygisk = false;
        }
    }
    delete plt_hook_list;
    plt_hook_list = nullptr;
    if (!lsplt::CommitHook()) {
        ZLOGE("Failed to restore plt_hook\n");
        should_unmap_zygisk = false;
    }
}

// -----------------------------------------------------------------

static JNINativeMethod *hookAndSaveJNIMethods(const char *, const JNINativeMethod *, int);

static string get_class_name(JNIEnv *env, jclass clazz) {
    static auto class_getName = env->GetMethodID(
            env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
    auto nameRef = (jstring) env->CallObjectMethod(clazz, class_getName);
    const char *name = env->GetStringUTFChars(nameRef, nullptr);
    string className(name);
    env->ReleaseStringUTFChars(nameRef, name);
    std::replace(className.begin(), className.end(), '.', '/');
    return className;
}

static jint env_RegisterNatives(
        JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint numMethods) {
    auto className = get_class_name(env, clazz);
    ZLOGV("JNIEnv->RegisterNatives [%s]\n", className.data());
    auto newMethods = unique_ptr<JNINativeMethod[]>(
            hookAndSaveJNIMethods(className.data(), methods, numMethods));
    return old_functions->RegisterNatives(env, clazz, newMethods.get() ?: methods, numMethods);
}

static void hook_jni_env() {
    using method_sig = jint(*)(JavaVM **, jsize, jsize *);
    auto get_created_vms = reinterpret_cast<method_sig>(
            dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!get_created_vms) {
        for (auto &map: lsplt::MapInfo::Scan()) {
            if (!map.path.ends_with("/libnativehelper.so")) continue;
            void *h = dlopen(map.path.data(), RTLD_LAZY);
            if (!h) {
                ZLOGW("Cannot dlopen libnativehelper.so: %s\n", dlerror());
                break;
            }
            get_created_vms = reinterpret_cast<method_sig>(dlsym(h, "JNI_GetCreatedJavaVMs"));
            dlclose(h);
            break;
        }
        if (!get_created_vms) {
            ZLOGW("JNI_GetCreatedJavaVMs not found\n");
            return;
        }
    }

    JavaVM *vm = nullptr;
    jsize num = 0;
    jint res = get_created_vms(&vm, 1, &num);
    if (res != JNI_OK || vm == nullptr) {
        ZLOGW("JavaVM not found\n");
        return;
    }
    JNIEnv *env = nullptr;
    res = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (res != JNI_OK || env == nullptr) {
        ZLOGW("JNIEnv not found\n");
        return;
    }

    // Replace the function table in JNIEnv to hook RegisterNatives
    default_new(new_functions);
    memcpy(new_functions, env->functions, sizeof(*new_functions));
    new_functions->RegisterNatives = &env_RegisterNatives;
    old_functions = env->functions;
    env->functions = new_functions;
}

static void restore_jni_env(JNIEnv *env) {
    env->functions = old_functions;
    delete new_functions;
    new_functions = nullptr;
}

#define HOOK_JNI(method)                                                                     \
if (methods[i].name == #method##sv) {                                                        \
    int j = 0;                                                                               \
    for (; j < method##_methods_num; ++j) {                                                  \
        if (strcmp(methods[i].signature, method##_methods[j].signature) == 0) {              \
            jni_hook_list->try_emplace(className).first->second.push_back(methods[i]);       \
            method##_orig = methods[i].fnPtr;                                                \
            newMethods[i] = method##_methods[j];                                             \
            ZLOGI("replaced %s#" #method "\n", className);                                   \
            --hook_cnt;                                                                      \
            break;                                                                           \
        }                                                                                    \
    }                                                                                        \
    if (j == method##_methods_num) {                                                         \
        ZLOGE("unknown signature of %s#" #method ": %s\n", className, methods[i].signature); \
    }                                                                                        \
    continue;                                                                                \
}

// JNI method hook definitions, auto generated
#include "jni_hooks.hpp"

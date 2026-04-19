#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "core_api.h"

typedef int  (*core_init_fn)(const ProxyContext *);
typedef void (*core_shutdown_fn)(void);

#define PROXY_CORE_NAME  L"DollmanMuteCore.dll"
#define PROXY_MUTEX_NAME L"Local\\DollmanMuteProxyInstance"

static HMODULE g_self_module = NULL;
static HMODULE g_core_module = NULL;
static core_init_fn g_core_init = NULL;
static core_shutdown_fn g_core_shutdown = NULL;

static CRITICAL_SECTION g_proxy_log_lock;
static BOOL g_proxy_log_lock_inited = FALSE;
static CRITICAL_SECTION g_core_lock;
static BOOL g_core_lock_inited = FALSE;

static char g_game_root[MAX_PATH];
static char g_core_path[MAX_PATH];
static wchar_t g_core_path_w[MAX_PATH];
static char g_ini_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static char g_proxy_log_path[MAX_PATH];
static wchar_t g_reload_flag_path_w[MAX_PATH];
static wchar_t g_unload_flag_path_w[MAX_PATH];
static wchar_t g_load_flag_path_w[MAX_PATH];
static ProxyContext g_ctx;

static HANDLE g_proxy_mutex = NULL;
static HANDLE g_reload_thread = NULL;
static volatile LONG g_proxy_exiting = 0;

static void proxy_log_v(const char *fmt, va_list args)
{
    char message[1024];
    char line[1200];
    SYSTEMTIME st;
    HANDLE file;
    DWORD written = 0;
    int message_len;
    int line_len;

    if (g_proxy_log_path[0] == '\0' || fmt == NULL) {
        return;
    }

    message_len = vsnprintf(message, sizeof(message), fmt, args);
    if (message_len < 0) {
        return;
    }

    GetLocalTime(&st);
    line_len = snprintf(
        line,
        sizeof(line),
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        (unsigned int)st.wYear,
        (unsigned int)st.wMonth,
        (unsigned int)st.wDay,
        (unsigned int)st.wHour,
        (unsigned int)st.wMinute,
        (unsigned int)st.wSecond,
        (unsigned int)st.wMilliseconds,
        message);
    if (line_len <= 0) {
        return;
    }

    if (g_proxy_log_lock_inited) {
        EnterCriticalSection(&g_proxy_log_lock);
    }
    file = CreateFileA(
        g_proxy_log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, (DWORD)line_len, &written, NULL);
        CloseHandle(file);
    }
    if (g_proxy_log_lock_inited) {
        LeaveCriticalSection(&g_proxy_log_lock);
    }
}

static void proxy_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    proxy_log_v(fmt, args);
    va_end(args);
}

static void join_path(char *buffer, size_t buffer_size, const char *dir, const char *file_name)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    if (dir == NULL || dir[0] == '\0') {
        snprintf(buffer, buffer_size, "%s", file_name != NULL ? file_name : "");
        return;
    }
    snprintf(buffer, buffer_size, "%s\\%s", dir, file_name != NULL ? file_name : "");
}

static void fill_paths(void)
{
    char module_path[MAX_PATH];
    char reload_flag_path[MAX_PATH];
    char unload_flag_path[MAX_PATH];
    char load_flag_path[MAX_PATH];
    char *last_slash;
    int i;

    g_game_root[0] = '\0';
    g_core_path[0] = '\0';
    g_core_path_w[0] = L'\0';
    g_ini_path[0] = '\0';
    g_log_path[0] = '\0';
    g_proxy_log_path[0] = '\0';
    g_reload_flag_path_w[0] = L'\0';
    g_unload_flag_path_w[0] = L'\0';
    g_load_flag_path_w[0] = L'\0';

    if (g_self_module == NULL) {
        return;
    }

    module_path[0] = '\0';
    GetModuleFileNameA(g_self_module, module_path, MAX_PATH);
    module_path[MAX_PATH - 1] = '\0';

    last_slash = strrchr(module_path, '\\');
    if (last_slash != NULL) {
        *last_slash = '\0';
    }

    snprintf(g_game_root, sizeof(g_game_root), "%s", module_path);
    join_path(g_core_path, sizeof(g_core_path), g_game_root, "DollmanMuteCore.dll");
    join_path(g_ini_path, sizeof(g_ini_path), g_game_root, "DollmanMute.ini");
    join_path(g_log_path, sizeof(g_log_path), g_game_root, "DollmanMute.log");
    join_path(g_proxy_log_path, sizeof(g_proxy_log_path), g_game_root, "DollmanMute.proxy.log");
    join_path(reload_flag_path, sizeof(reload_flag_path), g_game_root, "DollmanMute.reload");
    join_path(unload_flag_path, sizeof(unload_flag_path), g_game_root, "DollmanMute.unload");
    join_path(load_flag_path, sizeof(load_flag_path), g_game_root, "DollmanMute.load");

    for (i = 0; i < MAX_PATH && g_core_path[i] != '\0'; ++i) {
        g_core_path_w[i] = (wchar_t)(unsigned char)g_core_path[i];
    }
    g_core_path_w[i] = L'\0';

    for (i = 0; i < MAX_PATH && reload_flag_path[i] != '\0'; ++i) {
        g_reload_flag_path_w[i] = (wchar_t)(unsigned char)reload_flag_path[i];
    }
    g_reload_flag_path_w[i] = L'\0';

    for (i = 0; i < MAX_PATH && unload_flag_path[i] != '\0'; ++i) {
        g_unload_flag_path_w[i] = (wchar_t)(unsigned char)unload_flag_path[i];
    }
    g_unload_flag_path_w[i] = L'\0';

    for (i = 0; i < MAX_PATH && load_flag_path[i] != '\0'; ++i) {
        g_load_flag_path_w[i] = (wchar_t)(unsigned char)load_flag_path[i];
    }
    g_load_flag_path_w[i] = L'\0';
}

static BOOL acquire_proxy_mutex(void)
{
    g_proxy_mutex = CreateMutexW(NULL, FALSE, PROXY_MUTEX_NAME);
    if (g_proxy_mutex == NULL) {
        proxy_log("CreateMutexW failed: %lu", (unsigned long)GetLastError());
        return TRUE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        proxy_log("Another DollmanMute proxy already active; skipping duplicate init");
        return FALSE;
    }
    return TRUE;
}

static void fill_ctx(void)
{
    ZeroMemory(&g_ctx, sizeof(g_ctx));
    g_ctx.game_root = g_game_root;
    g_ctx.ini_path = g_ini_path;
    g_ctx.log_path = g_log_path;
    g_ctx.proxy_version = DOLLMANMUTE_PROXY_VERSION;
}

static BOOL load_core_locked(void)
{
    HMODULE mod;
    core_init_fn init_fn;
    core_shutdown_fn shutdown_fn;
    int init_result;

    if (g_core_module != NULL) {
        proxy_log("load_core: core already loaded");
        return TRUE;
    }

    mod = LoadLibraryW(g_core_path_w);
    if (mod == NULL) {
        proxy_log("LoadLibraryW(%s) failed: %lu", g_core_path, (unsigned long)GetLastError());
        return FALSE;
    }

    init_fn = (core_init_fn)GetProcAddress(mod, "core_init");
    shutdown_fn = (core_shutdown_fn)GetProcAddress(mod, "core_shutdown");
    if (init_fn == NULL || shutdown_fn == NULL) {
        proxy_log("core_init=%p core_shutdown=%p — export missing",
                  (void *)init_fn, (void *)shutdown_fn);
        FreeLibrary(mod);
        return FALSE;
    }

    g_core_module = mod;
    g_core_init = init_fn;
    g_core_shutdown = shutdown_fn;

    init_result = g_core_init(&g_ctx);
    proxy_log("core_init returned %d (module=%p)", init_result, (void *)mod);
    return TRUE;
}

static void unload_core_locked(void)
{
    HMODULE mod;
    HMODULE verify;

    if (g_core_module == NULL) {
        proxy_log("unload_core: no core loaded");
        return;
    }

    if (g_core_shutdown != NULL) {
        g_core_shutdown();
    }

    mod = g_core_module;
    g_core_module = NULL;
    g_core_init = NULL;
    g_core_shutdown = NULL;

    if (!FreeLibrary(mod)) {
        proxy_log("FreeLibrary failed: %lu", (unsigned long)GetLastError());
        return;
    }

    verify = GetModuleHandleW(PROXY_CORE_NAME);
    if (verify != NULL) {
        proxy_log("warning: core module still mapped after FreeLibrary (handle=%p)", (void *)verify);
    } else {
        proxy_log("core unloaded cleanly");
    }
}

static BOOL reload_core(void)
{
    BOOL ok;

    if (!g_core_lock_inited) {
        return FALSE;
    }

    EnterCriticalSection(&g_core_lock);
    proxy_log("F10: reloading core");
    unload_core_locked();
    Sleep(100);
    ok = load_core_locked();
    LeaveCriticalSection(&g_core_lock);

    proxy_log("F10: reload %s", ok ? "ok" : "failed");
    return ok;
}

static BOOL initial_load_core(void)
{
    BOOL ok;

    if (!g_core_lock_inited) {
        return FALSE;
    }

    EnterCriticalSection(&g_core_lock);
    ok = load_core_locked();
    LeaveCriticalSection(&g_core_lock);

    if (!ok) {
        proxy_log("initial load failed; press F10 after placing DollmanMuteCore.dll to retry");
    }
    return ok;
}

static DWORD WINAPI reload_thread_proc(LPVOID parameter)
{
    BOOL f10_prev = FALSE;
    int file_poll_counter = 0;
    (void)parameter;

    while (InterlockedCompareExchange(&g_proxy_exiting, 0, 0) == 0) {
        BOOL f10_now = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10_now && !f10_prev) {
            reload_core();
        }
        f10_prev = f10_now;

        /* Poll flag files every ~250ms (every 8 * 30ms iterations) */
        if (++file_poll_counter >= 8) {
            file_poll_counter = 0;

            if (g_unload_flag_path_w[0] != L'\0' &&
                GetFileAttributesW(g_unload_flag_path_w) != INVALID_FILE_ATTRIBUTES) {
                if (DeleteFileW(g_unload_flag_path_w)) {
                    proxy_log("File trigger: DollmanMute.unload consumed");
                    if (g_core_lock_inited) {
                        EnterCriticalSection(&g_core_lock);
                        unload_core_locked();
                        LeaveCriticalSection(&g_core_lock);
                    }
                }
            }

            if (g_load_flag_path_w[0] != L'\0' &&
                GetFileAttributesW(g_load_flag_path_w) != INVALID_FILE_ATTRIBUTES) {
                if (DeleteFileW(g_load_flag_path_w)) {
                    proxy_log("File trigger: DollmanMute.load consumed");
                    if (g_core_lock_inited) {
                        EnterCriticalSection(&g_core_lock);
                        load_core_locked();
                        LeaveCriticalSection(&g_core_lock);
                    }
                }
            }

            if (g_reload_flag_path_w[0] != L'\0' &&
                GetFileAttributesW(g_reload_flag_path_w) != INVALID_FILE_ATTRIBUTES) {
                if (DeleteFileW(g_reload_flag_path_w)) {
                    proxy_log("File trigger: DollmanMute.reload consumed");
                    reload_core();
                }
            }
        }

        Sleep(30);
    }
    return 0;
}

static DWORD WINAPI bootstrap_thread_proc(LPVOID parameter)
{
    (void)parameter;

    fill_paths();
    if (g_proxy_log_path[0] != '\0') {
        proxy_log("DollmanMute proxy start: self=%s", g_game_root);
        proxy_log("core path=%s", g_core_path);
    }

    if (!acquire_proxy_mutex()) {
        return 0;
    }

    fill_ctx();
    initial_load_core();

    g_reload_thread = CreateThread(NULL, 0, reload_thread_proc, NULL, 0, NULL);
    if (g_reload_thread == NULL) {
        proxy_log("CreateThread(reload_thread) failed: %lu", (unsigned long)GetLastError());
    } else {
        proxy_log("Reload armed: F10 key OR create DollmanMute.reload file");
    }

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    HANDLE boot;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_proxy_log_lock);
        g_proxy_log_lock_inited = TRUE;
        InitializeCriticalSection(&g_core_lock);
        g_core_lock_inited = TRUE;

        boot = CreateThread(NULL, 0, bootstrap_thread_proc, NULL, 0, NULL);
        if (boot != NULL) {
            CloseHandle(boot);
        }
    }

    return TRUE;
}

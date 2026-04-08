#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "MinHook.h"

typedef uint32_t AkUniqueID;
typedef uint32_t AkPlayingID;
typedef uint64_t AkGameObjectID;
typedef int32_t AKRESULT;

typedef struct Config {
    BOOL enabled;
    BOOL verbose_log;
} Config;

typedef AkPlayingID(__cdecl *PostEventIdFn)(
    AkUniqueID event_id,
    AkGameObjectID game_object_id,
    uint32_t callback_mask,
    void *callback,
    void *cookie,
    uint32_t external_source_count,
    void *external_sources,
    uint32_t playing_id);
typedef uintptr_t(__fastcall *GenericVoiceFn5)(
    const void *arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uintptr_t arg4,
    uintptr_t arg5);
typedef uintptr_t(__fastcall *GenericVoiceFn7)(
    const void *arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uintptr_t arg4,
    uintptr_t arg5,
    uintptr_t arg6,
    uintptr_t arg7);

static HMODULE g_self_module = NULL;
static CRITICAL_SECTION g_log_lock;
static Config g_cfg;
static char g_ini_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static BOOL g_hooks_installed = FALSE;

static PostEventIdFn g_real_post_event_id = NULL;
static GenericVoiceFn5 g_real_play_voice_impl = NULL;
static GenericVoiceFn7 g_real_play_voice_with_sentence_impl = NULL;
static GenericVoiceFn7 g_real_play_voice_with_sentence_randomly_impl = NULL;

static const char *k_build_tag = "public-release-v1";

static const char *k_export_post_event_id =
    "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";

static const uintptr_t k_rva_play_voice_impl = 0x00D8B390u;
static const uintptr_t k_rva_play_voice_with_sentence_impl = 0x00D8B5B0u;
static const uintptr_t k_rva_play_voice_with_sentence_randomly_impl = 0x00D8B870u;

/*
 * These IDs are the currently confirmed Dollman Wwise voice events.
 * They are blocked directly without any heuristic windows or runtime learning.
 */
static const AkUniqueID k_blocked_event_ids[] = {
    2995625663u, /* equip */
    2820786646u, /* throw */
    2978848044u, /* throw / return */
    1966841225u, /* older random chatter candidate */
    302733266u   /* task-failure line */
};

/*
 * These hashes are the currently confirmed Dollman PlayerVoice entries.
 * Blocking here catches random chatter before it reaches the Wwise layer.
 */
static const uint32_t k_blocked_voice_hashes[] = {
    0x2cf2ead9u, /* load-save library prompt */
    0x3f8a5e65u, /* throw / return */
    0x6fdbf8b5u, /* pre-whoosh chatter */
    0x7bd37461u, /* return sequence */
    0x6023776du, /* second whoosh sequence */
    0x50b55cfau /* random chatter */
};

static const char *k_default_ini =
    "; DollmanMute public release config\n"
    "; Enabled=1 keeps Dollman voice muted.\n"
    "; Set VerboseLog=1 only when gathering debugging info.\n"
    "\n"
    "[General]\n"
    "Enabled=1\n"
    "VerboseLog=0\n";

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

static void init_paths(void)
{
    char module_path[MAX_PATH];
    char *last_slash = NULL;

    module_path[0] = '\0';
    if (g_self_module == NULL) {
        return;
    }

    GetModuleFileNameA(g_self_module, module_path, MAX_PATH);
    module_path[MAX_PATH - 1] = '\0';

    last_slash = strrchr(module_path, '\\');
    if (last_slash != NULL) {
        *last_slash = '\0';
    }

    join_path(g_ini_path, sizeof(g_ini_path), module_path, "DollmanMute.ini");
    join_path(g_log_path, sizeof(g_log_path), module_path, "DollmanMute.log");
}

static void ensure_default_ini(void)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0;

    if (g_ini_path[0] == '\0' || GetFileAttributesA(g_ini_path) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    file = CreateFileA(
        g_ini_path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    WriteFile(file, k_default_ini, (DWORD)strlen(k_default_ini), &written, NULL);
    CloseHandle(file);
}

static void load_config(void)
{
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    g_cfg.enabled = TRUE;
    g_cfg.verbose_log = FALSE;

    ensure_default_ini();

    g_cfg.enabled = GetPrivateProfileIntA("General", "Enabled", g_cfg.enabled, g_ini_path) != 0;
    g_cfg.verbose_log = GetPrivateProfileIntA("General", "VerboseLog", g_cfg.verbose_log, g_ini_path) != 0;
}

static void write_log_v(const char *fmt, va_list args)
{
    char message[1024];
    char line[1200];
    SYSTEMTIME st;
    HANDLE file;
    DWORD written = 0;
    int message_len;
    int line_len;

    if (g_log_path[0] == '\0' || fmt == NULL) {
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

    EnterCriticalSection(&g_log_lock);
    file = CreateFileA(
        g_log_path,
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
    LeaveCriticalSection(&g_log_lock);
}

static void log_line(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    write_log_v(fmt, args);
    va_end(args);
}

static void log_verbose(const char *fmt, ...)
{
    va_list args;

    if (!g_cfg.verbose_log) {
        return;
    }

    va_start(args, fmt);
    write_log_v(fmt, args);
    va_end(args);
}

static BOOL should_block_event_id(AkUniqueID event_id)
{
    size_t i;

    for (i = 0; i < sizeof(k_blocked_event_ids) / sizeof(k_blocked_event_ids[0]); ++i) {
        if (k_blocked_event_ids[i] == event_id) {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL should_block_voice_hash(uint32_t hash_value)
{
    size_t i;

    if (hash_value == 0) {
        return FALSE;
    }

    for (i = 0; i < sizeof(k_blocked_voice_hashes) / sizeof(k_blocked_voice_hashes[0]); ++i) {
        if (k_blocked_voice_hashes[i] == hash_value) {
            return TRUE;
        }
    }

    return FALSE;
}

static void *resolve_export(const char *name)
{
    HMODULE exe_module = GetModuleHandleW(NULL);

    if (exe_module == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }

    return (void *)GetProcAddress(exe_module, name);
}

static void *resolve_rva(uintptr_t rva)
{
    HMODULE exe_module = GetModuleHandleW(NULL);

    if (exe_module == NULL || rva == 0) {
        return NULL;
    }

    return (void *)((uintptr_t)exe_module + rva);
}

static BOOL install_hook(void *target, void *detour, void **original, const char *label)
{
    MH_STATUS status;

    if (target == NULL) {
        log_line("Skip hook %s: target not found", label != NULL ? label : "(unknown)");
        return FALSE;
    }

    status = MH_CreateHook(target, detour, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
        log_line("Failed to create hook %s: %d", label != NULL ? label : "(unknown)", (int)status);
        return FALSE;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        log_line("Failed to enable hook %s: %d", label != NULL ? label : "(unknown)", (int)status);
        return FALSE;
    }

    log_line("Hooked %s at %p", label != NULL ? label : "(unknown)", target);
    return TRUE;
}

static BOOL install_export_hook(const char *export_name, void *detour, void **original, const char *label)
{
    return install_hook(resolve_export(export_name), detour, original, label);
}

static BOOL install_rva_hook(uintptr_t rva, void *detour, void **original, const char *label)
{
    return install_hook(resolve_rva(rva), detour, original, label);
}

static AkPlayingID __cdecl hook_post_event_id(
    AkUniqueID event_id,
    AkGameObjectID game_object_id,
    uint32_t callback_mask,
    void *callback,
    void *cookie,
    uint32_t external_source_count,
    void *external_sources,
    uint32_t playing_id)
{
    if (g_cfg.enabled && should_block_event_id(event_id)) {
        log_verbose(
            "Blocked PostEventID eventId=%u gameObject=0x%llx externalSources=%u",
            event_id,
            (unsigned long long)game_object_id,
            (unsigned int)external_source_count);
        return 0;
    }

    return g_real_post_event_id(
        event_id,
        game_object_id,
        callback_mask,
        callback,
        cookie,
        external_source_count,
        external_sources,
        playing_id);
}

static uintptr_t __fastcall hook_play_voice_impl(
    const void *arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uintptr_t arg4,
    uintptr_t arg5)
{
    uint32_t hash_value = (uint32_t)arg2;
    (void)arg1;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (g_cfg.enabled && should_block_voice_hash(hash_value)) {
        log_verbose("Blocked PlayerVoice.Play hash=0x%08x", (unsigned int)hash_value);
        return 0;
    }

    return g_real_play_voice_impl(arg1, arg2, arg3, arg4, arg5);
}

static uintptr_t __fastcall hook_play_voice_with_sentence_impl(
    const void *arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uintptr_t arg4,
    uintptr_t arg5,
    uintptr_t arg6,
    uintptr_t arg7)
{
    uint32_t hash_value = (uint32_t)arg2;
    (void)arg1;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)arg6;
    (void)arg7;

    if (g_cfg.enabled && should_block_voice_hash(hash_value)) {
        log_verbose("Blocked PlayerVoice.PlayWithSentence hash=0x%08x", (unsigned int)hash_value);
        return 0;
    }

    return g_real_play_voice_with_sentence_impl(arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

static uintptr_t __fastcall hook_play_voice_with_sentence_randomly_impl(
    const void *arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uintptr_t arg4,
    uintptr_t arg5,
    uintptr_t arg6,
    uintptr_t arg7)
{
    uint32_t hash_value = (uint32_t)arg2;
    (void)arg1;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)arg6;
    (void)arg7;

    if (g_cfg.enabled && should_block_voice_hash(hash_value)) {
        log_verbose("Blocked PlayerVoice.PlayWithSentenceRandomly hash=0x%08x", (unsigned int)hash_value);
        return 0;
    }

    return g_real_play_voice_with_sentence_randomly_impl(arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

static DWORD WINAPI initialize_thread_proc(LPVOID parameter)
{
    MH_STATUS status;
    unsigned int hook_count = 0;

    (void)parameter;

    init_paths();
    load_config();

    log_line("DollmanMute build: %s", k_build_tag);
    log_line(
        "DollmanMute init start: enabled=%d verbose=%d",
        g_cfg.enabled,
        g_cfg.verbose_log);

    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("MH_Initialize failed: %d", (int)status);
        return 0;
    }

    if (install_export_hook(k_export_post_event_id, hook_post_event_id, (void **)&g_real_post_event_id, "PostEventID")) {
        ++hook_count;
    }
    if (install_rva_hook(k_rva_play_voice_impl, hook_play_voice_impl, (void **)&g_real_play_voice_impl, "PlayVoiceImpl")) {
        ++hook_count;
    }
    if (install_rva_hook(k_rva_play_voice_with_sentence_impl, hook_play_voice_with_sentence_impl, (void **)&g_real_play_voice_with_sentence_impl, "PlayVoiceWithSentenceImpl")) {
        ++hook_count;
    }
    if (install_rva_hook(k_rva_play_voice_with_sentence_randomly_impl, hook_play_voice_with_sentence_randomly_impl, (void **)&g_real_play_voice_with_sentence_randomly_impl, "PlayVoiceWithSentenceRandomlyImpl")) {
        ++hook_count;
    }

    g_hooks_installed = hook_count != 0;
    log_line("DollmanMute init complete: hooks=%u", hook_count);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    HANDLE thread_handle;

    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    g_self_module = instance;
    InitializeCriticalSection(&g_log_lock);
    DisableThreadLibraryCalls(instance);

    thread_handle = CreateThread(NULL, 0, initialize_thread_proc, NULL, 0, NULL);
    if (thread_handle != NULL) {
        CloseHandle(thread_handle);
    }

    return TRUE;
}

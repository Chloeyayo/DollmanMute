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
    BOOL enable_player_voice_hooks;
    BOOL enable_mark_hotkey;
    uint32_t scanner_mode;
    uint32_t mark_hotkey_virtual_key;
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
typedef AkUniqueID(__cdecl *GetIdFromStringAFn)(const char *name);
typedef AkUniqueID(__cdecl *GetIdFromStringWFn)(const wchar_t *name);
typedef AkPlayingID(__cdecl *PostEventNameAFn)(
    const char *event_name,
    AkGameObjectID game_object_id,
    uint32_t callback_mask,
    void *callback,
    void *cookie,
    uint32_t external_source_count,
    void *external_sources,
    uint32_t playing_id);
typedef AkPlayingID(__cdecl *PostEventNameWFn)(
    const wchar_t *event_name,
    AkGameObjectID game_object_id,
    uint32_t callback_mask,
    void *callback,
    void *cookie,
    uint32_t external_source_count,
    void *external_sources,
    uint32_t playing_id);
typedef AKRESULT(__cdecl *SetStateAFn)(const char *group_name, const char *state_name);
typedef AKRESULT(__cdecl *SetStateWFn)(const wchar_t *group_name, const wchar_t *state_name);
typedef AKRESULT(__cdecl *SetStateIdFn)(AkUniqueID group_id, AkUniqueID state_id);
typedef AKRESULT(__cdecl *SetSwitchAFn)(const char *group_name, const char *state_name, AkGameObjectID game_object_id);
typedef AKRESULT(__cdecl *SetSwitchWFn)(const wchar_t *group_name, const wchar_t *state_name, AkGameObjectID game_object_id);
typedef AKRESULT(__cdecl *SetSwitchIdFn)(AkUniqueID group_id, AkUniqueID state_id, AkGameObjectID game_object_id);
typedef AKRESULT(__cdecl *PostTriggerAFn)(const char *trigger_name, AkGameObjectID game_object_id);
typedef AKRESULT(__cdecl *PostTriggerWFn)(const wchar_t *trigger_name, AkGameObjectID game_object_id);
typedef AKRESULT(__cdecl *PostTriggerIdFn)(AkUniqueID trigger_id, AkGameObjectID game_object_id);
typedef AKRESULT(__cdecl *SetRtpcValueAFn)(const char *rtpc_name, float value, AkGameObjectID game_object_id, int32_t interpolation_time_ms, int32_t fade_curve, BOOL bypass_game_param);
typedef AKRESULT(__cdecl *SetRtpcValueWFn)(const wchar_t *rtpc_name, float value, AkGameObjectID game_object_id, int32_t interpolation_time_ms, int32_t fade_curve, BOOL bypass_game_param);
typedef AKRESULT(__cdecl *SetRtpcValueIdFn)(AkUniqueID rtpc_id, float value, AkGameObjectID game_object_id, int32_t interpolation_time_ms, int32_t fade_curve, BOOL bypass_game_param);
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
static CRITICAL_SECTION g_trace_lock;
static Config g_cfg;
static char g_ini_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static BOOL g_hooks_installed = FALSE;
static volatile BOOL g_mark_hotkey_thread_running = FALSE;

static PostEventIdFn g_real_post_event_id = NULL;
static GetIdFromStringAFn g_real_get_id_from_string_a = NULL;
static GetIdFromStringWFn g_real_get_id_from_string_w = NULL;
static PostEventNameAFn g_real_post_event_name_a = NULL;
static PostEventNameWFn g_real_post_event_name_w = NULL;
static SetStateAFn g_real_set_state_a = NULL;
static SetStateWFn g_real_set_state_w = NULL;
static SetStateIdFn g_real_set_state_id = NULL;
static SetSwitchAFn g_real_set_switch_a = NULL;
static SetSwitchWFn g_real_set_switch_w = NULL;
static SetSwitchIdFn g_real_set_switch_id = NULL;
static PostTriggerAFn g_real_post_trigger_a = NULL;
static PostTriggerWFn g_real_post_trigger_w = NULL;
static PostTriggerIdFn g_real_post_trigger_id = NULL;
static SetRtpcValueAFn g_real_set_rtpc_value_a = NULL;
static SetRtpcValueWFn g_real_set_rtpc_value_w = NULL;
static SetRtpcValueIdFn g_real_set_rtpc_value_id = NULL;
static GenericVoiceFn5 g_real_play_voice_impl = NULL;
static GenericVoiceFn7 g_real_play_voice_with_sentence_impl = NULL;
static GenericVoiceFn7 g_real_play_voice_with_sentence_randomly_impl = NULL;

static const char *k_build_tag = "public-release-v1.2";

static const char *k_export_post_event_id =
    "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";
static const char *k_export_get_id_from_string_a =
    "?GetIDFromString@SoundEngine@AK@@YAIPEBD@Z";
static const char *k_export_get_id_from_string_w =
    "?GetIDFromString@SoundEngine@AK@@YAIPEB_W@Z";
static const char *k_export_post_event_name_a =
    "?PostEvent@SoundEngine@AK@@YAIPEBD_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";
static const char *k_export_post_event_name_w =
    "?PostEvent@SoundEngine@AK@@YAIPEB_W_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";
static const char *k_export_set_state_a =
    "?SetState@SoundEngine@AK@@YA?AW4AKRESULT@@PEBD0@Z";
static const char *k_export_set_state_w =
    "?SetState@SoundEngine@AK@@YA?AW4AKRESULT@@PEB_W0@Z";
static const char *k_export_set_state_id =
    "?SetState@SoundEngine@AK@@YA?AW4AKRESULT@@II@Z";
static const char *k_export_set_switch_a =
    "?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@PEBD0_K@Z";
static const char *k_export_set_switch_w =
    "?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@PEB_W0_K@Z";
static const char *k_export_set_switch_id =
    "?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@II_K@Z";
static const char *k_export_post_trigger_a =
    "?PostTrigger@SoundEngine@AK@@YA?AW4AKRESULT@@PEBD_K@Z";
static const char *k_export_post_trigger_w =
    "?PostTrigger@SoundEngine@AK@@YA?AW4AKRESULT@@PEB_W_K@Z";
static const char *k_export_post_trigger_id =
    "?PostTrigger@SoundEngine@AK@@YA?AW4AKRESULT@@I_K@Z";
static const char *k_export_set_rtpc_value_a =
    "?SetRTPCValue@SoundEngine@AK@@YA?AW4AKRESULT@@PEBDM_KHW4AkCurveInterpolation@@_N@Z";
static const char *k_export_set_rtpc_value_w =
    "?SetRTPCValue@SoundEngine@AK@@YA?AW4AKRESULT@@PEB_WM_KHW4AkCurveInterpolation@@_N@Z";
static const char *k_export_set_rtpc_value_id =
    "?SetRTPCValue@SoundEngine@AK@@YA?AW4AKRESULT@@IM_KHW4AkCurveInterpolation@@_N@Z";

static const uintptr_t k_rva_play_voice_impl = 0x00D8B390u;
static const uintptr_t k_rva_play_voice_with_sentence_impl = 0x00D8B5B0u;
static const uintptr_t k_rva_play_voice_with_sentence_randomly_impl = 0x00D8B870u;
static const AkUniqueID k_scanner_event_id_1 = 4235852663u;
static const AkUniqueID k_scanner_event_id_2 = 4094913469u;
static const AkUniqueID k_scanner_event_id_3 = 2611919341u;
enum {
    SCANNER_MODE_OFF = 0u,
    SCANNER_MODE_REDUCED = 1u,
    SCANNER_MODE_MUTE_ALL = 2u
};

/*
 * These IDs are the currently confirmed Dollman Wwise voice events.
 * They are blocked directly without any heuristic windows or runtime learning.
 */
static const AkUniqueID k_blocked_event_ids[] = {
    2995625663u, /* equip */
    2820786646u, /* throw */
    2978848044u, /* throw / return */
    1966841225u, /* older random chatter candidate */
    302733266u,  /* task-failure line */
    2417186760u, /* near-fall chatter candidate: rejected after runtime trace */
    448888368u   /* near-fall chatter candidate: tail-window single-shot external source */
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
    "; PlayerVoice hooks are disabled by default on updated game builds\n"
    "; because those internal offsets may change and cause crashes.\n"
    "; EnableMarkHotkey=1 lets you press F10 to drop a MARK line into the log.\n"
    "; ScannerMode is a 3-level scanner audio setting.\n"
    "; ScannerMode=0 keeps scanner audio unchanged.\n"
    "; ScannerMode=1 reduces scanner intensity:\n"
    ";   keeps 4235852663, blocks 4094913469 and 2611919341.\n"
    "; ScannerMode=2 fully mutes scanner audio:\n"
    ";   blocks 4235852663, 4094913469, and 2611919341.\n"
    "\n"
    "[General]\n"
    "Enabled=1\n"
    "VerboseLog=0\n"
    "EnablePlayerVoiceHooks=0\n"
    "EnableMarkHotkey=1\n"
    "MarkHotkeyVirtualKey=121\n"
    "ScannerMode=0\n";

static uint32_t g_seen_trace_hashes[4096];
static size_t g_seen_trace_count = 0;
static AkUniqueID g_traced_event_ids[1024];
static size_t g_traced_event_id_count = 0;

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
    g_cfg.enable_player_voice_hooks = FALSE;
    g_cfg.enable_mark_hotkey = TRUE;
    g_cfg.scanner_mode = SCANNER_MODE_OFF;
    g_cfg.mark_hotkey_virtual_key = 121u;

    ensure_default_ini();

    g_cfg.enabled = GetPrivateProfileIntA("General", "Enabled", g_cfg.enabled, g_ini_path) != 0;
    g_cfg.verbose_log = GetPrivateProfileIntA("General", "VerboseLog", g_cfg.verbose_log, g_ini_path) != 0;
    g_cfg.enable_player_voice_hooks = GetPrivateProfileIntA("General", "EnablePlayerVoiceHooks", g_cfg.enable_player_voice_hooks, g_ini_path) != 0;
    g_cfg.enable_mark_hotkey = GetPrivateProfileIntA("General", "EnableMarkHotkey", g_cfg.enable_mark_hotkey, g_ini_path) != 0;
    g_cfg.mark_hotkey_virtual_key = (uint32_t)GetPrivateProfileIntA("General", "MarkHotkeyVirtualKey", (int)g_cfg.mark_hotkey_virtual_key, g_ini_path);
    {
        int scanner_mode_value = GetPrivateProfileIntA("General", "ScannerMode", (int)g_cfg.scanner_mode, g_ini_path);
        if (scanner_mode_value < (int)SCANNER_MODE_OFF) {
            scanner_mode_value = (int)SCANNER_MODE_OFF;
        }
        if (scanner_mode_value > (int)SCANNER_MODE_MUTE_ALL) {
            scanner_mode_value = (int)SCANNER_MODE_MUTE_ALL;
        }
        g_cfg.scanner_mode = (uint32_t)scanner_mode_value;
    }
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

static const char *virtual_key_name(uint32_t virtual_key)
{
    switch (virtual_key) {
    case VK_F1: return "F1";
    case VK_F2: return "F2";
    case VK_F3: return "F3";
    case VK_F4: return "F4";
    case VK_F5: return "F5";
    case VK_F6: return "F6";
    case VK_F7: return "F7";
    case VK_F8: return "F8";
    case VK_F9: return "F9";
    case VK_F10: return "F10";
    case VK_F11: return "F11";
    case VK_F12: return "F12";
    default: return "custom";
    }
}

static DWORD WINAPI mark_hotkey_thread_proc(LPVOID parameter)
{
    BOOL last_down = FALSE;
    uint32_t virtual_key = (uint32_t)(uintptr_t)parameter;

    g_mark_hotkey_thread_running = TRUE;
    log_line(
        "Mark hotkey thread started: key=%s vk=%u",
        virtual_key_name(virtual_key),
        (unsigned int)virtual_key);

    for (;;) {
        BOOL is_down = (GetAsyncKeyState((int)virtual_key) & 0x8000) != 0;
        if (is_down && !last_down) {
            log_line(
                "MARK key=%s vk=%u tick=%llu",
                virtual_key_name(virtual_key),
                (unsigned int)virtual_key,
                (unsigned long long)GetTickCount64());
        }

        last_down = is_down;
        Sleep(25);
    }
}

static char ascii_tolower_char(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static BOOL contains_case_insensitive(const char *text, const char *needle)
{
    size_t text_index;
    size_t needle_index;

    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return FALSE;
    }

    for (text_index = 0; text[text_index] != '\0'; ++text_index) {
        for (needle_index = 0;; ++needle_index) {
            char text_ch = ascii_tolower_char(text[text_index + needle_index]);
            char needle_ch = ascii_tolower_char(needle[needle_index]);

            if (needle_ch == '\0') {
                return TRUE;
            }
            if (text_ch == '\0' || text_ch != needle_ch) {
                break;
            }
        }
    }

    return FALSE;
}

static size_t copy_ascii_text(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0;

    if (dst == NULL || dst_size == 0) {
        return 0;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return 0;
    }

    while (src[i] != '\0' && i + 1 < dst_size) {
        unsigned char ch = (unsigned char)src[i];
        if (ch < 0x20 || ch > 0x7e) {
            dst[0] = '\0';
            return 0;
        }
        dst[i] = (char)ch;
        ++i;
    }

    if (i == 0 || src[i] != '\0') {
        dst[0] = '\0';
        return 0;
    }

    dst[i] = '\0';
    return i;
}

static size_t copy_ascii_wide_text(const wchar_t *src, char *dst, size_t dst_size)
{
    size_t i = 0;

    if (dst == NULL || dst_size == 0) {
        return 0;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return 0;
    }

    while (src[i] != L'\0' && i + 1 < dst_size) {
        wchar_t ch = src[i];
        if (ch < 0x20 || ch > 0x7e) {
            dst[0] = '\0';
            return 0;
        }
        dst[i] = (char)ch;
        ++i;
    }

    if (i == 0 || src[i] != L'\0') {
        dst[0] = '\0';
        return 0;
    }

    dst[i] = '\0';
    return i;
}

static BOOL has_trace_keyword(const char *text)
{
    static const char *k_keywords[] = {
        "dollman",
        "talk",
        "voice",
        "radio",
        "balance",
        "stagger",
        "fall",
        "slip",
        "trip",
        "shake",
        "wobble",
        "cargo",
        "playervoice"
    };
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return FALSE;
    }

    for (i = 0; i < sizeof(k_keywords) / sizeof(k_keywords[0]); ++i) {
        if (contains_case_insensitive(text, k_keywords[i])) {
            return TRUE;
        }
    }

    return FALSE;
}

static uint32_t hash_trace_key(const char *kind, const char *value1, const char *value2);
static BOOL reserve_trace_key(uint32_t trace_hash);

static void trace_numeric_once(const char *kind, AkUniqueID value1, AkUniqueID value2, AkGameObjectID game_object_id, float rtpc_value)
{
    char buffer1[32];
    char buffer2[32];

    if (!g_cfg.verbose_log) {
        return;
    }

    snprintf(buffer1, sizeof(buffer1), "%u", (unsigned int)value1);
    snprintf(buffer2, sizeof(buffer2), "%u", (unsigned int)value2);
    if (reserve_trace_key(hash_trace_key(kind, buffer1, buffer2))) {
        log_verbose(
            "Trace %s a=%u b=%u gameObject=0x%llx value=%.3f",
            kind,
            (unsigned int)value1,
            (unsigned int)value2,
            (unsigned long long)game_object_id,
            (double)rtpc_value);
    }
}

static uint32_t hash_trace_key(const char *kind, const char *value1, const char *value2)
{
    uint32_t hash = 2166136261u;
    const char *parts[3];
    size_t part_index;
    size_t char_index;

    parts[0] = kind != NULL ? kind : "";
    parts[1] = value1 != NULL ? value1 : "";
    parts[2] = value2 != NULL ? value2 : "";

    for (part_index = 0; part_index < 3; ++part_index) {
        for (char_index = 0; parts[part_index][char_index] != '\0'; ++char_index) {
            hash ^= (uint8_t)parts[part_index][char_index];
            hash *= 16777619u;
        }
        hash ^= (uint8_t)'|';
        hash *= 16777619u;
    }

    return hash;
}

static BOOL reserve_trace_key(uint32_t trace_hash)
{
    size_t i;
    BOOL should_log = FALSE;

    EnterCriticalSection(&g_trace_lock);
    for (i = 0; i < g_seen_trace_count; ++i) {
        if (g_seen_trace_hashes[i] == trace_hash) {
            LeaveCriticalSection(&g_trace_lock);
            return FALSE;
        }
    }

    if (g_seen_trace_count < sizeof(g_seen_trace_hashes) / sizeof(g_seen_trace_hashes[0])) {
        g_seen_trace_hashes[g_seen_trace_count++] = trace_hash;
        should_log = TRUE;
    }
    LeaveCriticalSection(&g_trace_lock);
    return should_log;
}

static void remember_traced_event_id(AkUniqueID event_id)
{
    size_t i;

    if (event_id == 0) {
        return;
    }

    EnterCriticalSection(&g_trace_lock);
    for (i = 0; i < g_traced_event_id_count; ++i) {
        if (g_traced_event_ids[i] == event_id) {
            LeaveCriticalSection(&g_trace_lock);
            return;
        }
    }

    if (g_traced_event_id_count < sizeof(g_traced_event_ids) / sizeof(g_traced_event_ids[0])) {
        g_traced_event_ids[g_traced_event_id_count++] = event_id;
    }
    LeaveCriticalSection(&g_trace_lock);
}

static BOOL is_traced_event_id(AkUniqueID event_id)
{
    size_t i;
    BOOL found = FALSE;

    if (event_id == 0) {
        return FALSE;
    }

    EnterCriticalSection(&g_trace_lock);
    for (i = 0; i < g_traced_event_id_count; ++i) {
        if (g_traced_event_ids[i] == event_id) {
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_trace_lock);
    return found;
}

static void trace_name_id_once(const char *kind, const char *name, AkUniqueID event_id)
{
    if (!g_cfg.verbose_log || name == NULL || name[0] == '\0') {
        return;
    }

    if (reserve_trace_key(hash_trace_key(kind, name, NULL))) {
        log_verbose("Trace %s name=%s id=%u", kind, name, (unsigned int)event_id);
    }
    remember_traced_event_id(event_id);
}

static void trace_name_once(const char *kind, const char *name, AkGameObjectID game_object_id)
{
    if (!g_cfg.verbose_log || name == NULL || name[0] == '\0') {
        return;
    }

    if (reserve_trace_key(hash_trace_key(kind, name, NULL))) {
        log_verbose(
            "Trace %s name=%s gameObject=0x%llx",
            kind,
            name,
            (unsigned long long)game_object_id);
    }
}

static void trace_pair_once(const char *kind, const char *value1, const char *value2, AkGameObjectID game_object_id)
{
    if (!g_cfg.verbose_log || ((value1 == NULL || value1[0] == '\0') && (value2 == NULL || value2[0] == '\0'))) {
        return;
    }

    if (reserve_trace_key(hash_trace_key(kind, value1, value2))) {
        log_verbose(
            "Trace %s a=%s b=%s gameObject=0x%llx",
            kind,
            value1 != NULL ? value1 : "(null)",
            value2 != NULL ? value2 : "(null)",
            (unsigned long long)game_object_id);
    }
}

static void trace_single_once(const char *kind, const char *value, AkGameObjectID game_object_id)
{
    if (!g_cfg.verbose_log || value == NULL || value[0] == '\0') {
        return;
    }

    if (reserve_trace_key(hash_trace_key(kind, value, NULL))) {
        log_verbose(
            "Trace %s name=%s gameObject=0x%llx",
            kind,
            value,
            (unsigned long long)game_object_id);
    }
}

static BOOL should_block_event_id(AkUniqueID event_id)
{
    size_t i;

    if (event_id == k_scanner_event_id_1) {
        return g_cfg.scanner_mode == SCANNER_MODE_MUTE_ALL;
    }
    if (event_id == k_scanner_event_id_2) {
        return g_cfg.scanner_mode == SCANNER_MODE_MUTE_ALL || g_cfg.scanner_mode == SCANNER_MODE_REDUCED;
    }
    if (event_id == k_scanner_event_id_3) {
        return g_cfg.scanner_mode == SCANNER_MODE_MUTE_ALL || g_cfg.scanner_mode == SCANNER_MODE_REDUCED;
    }

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
    BOOL blocked = g_cfg.enabled && should_block_event_id(event_id);

    if (g_cfg.verbose_log) {
        log_verbose(
            "%s PostEventID eventId=%u gameObject=0x%llx externalSources=%u",
            is_traced_event_id(event_id) ? "Trace" : "Seen",
            event_id,
            (unsigned long long)game_object_id,
            (unsigned int)external_source_count);
    }

    if (blocked) {
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

static AkUniqueID __cdecl hook_get_id_from_string_a(const char *name)
{
    char text[160];
    AkUniqueID event_id;

    event_id = g_real_get_id_from_string_a(name);
    if (copy_ascii_text(name, text, sizeof(text)) != 0) {
        trace_name_id_once("GetIDFromStringA", text, event_id);
    }
    return event_id;
}

static AkUniqueID __cdecl hook_get_id_from_string_w(const wchar_t *name)
{
    char text[160];
    AkUniqueID event_id;

    event_id = g_real_get_id_from_string_w(name);
    if (copy_ascii_wide_text(name, text, sizeof(text)) != 0) {
        trace_name_id_once("GetIDFromStringW", text, event_id);
    }
    return event_id;
}

static AkPlayingID __cdecl hook_post_event_name_a(
    const char *event_name,
    AkGameObjectID game_object_id,
    uint32_t callback_mask,
    void *callback,
    void *cookie,
    uint32_t external_source_count,
    void *external_sources,
    uint32_t playing_id)
{
    char text[160];

    if (copy_ascii_text(event_name, text, sizeof(text)) != 0) {
        trace_name_once("PostEventA", text, game_object_id);
    }

    return g_real_post_event_name_a(
        event_name,
        game_object_id,
        callback_mask,
        callback,
        cookie,
        external_source_count,
        external_sources,
        playing_id);
}

static AkPlayingID __cdecl hook_post_event_name_w(
    const wchar_t *event_name,
    AkGameObjectID game_object_id,
    uint32_t callback_mask,
    void *callback,
    void *cookie,
    uint32_t external_source_count,
    void *external_sources,
    uint32_t playing_id)
{
    char text[160];

    if (copy_ascii_wide_text(event_name, text, sizeof(text)) != 0) {
        trace_name_once("PostEventW", text, game_object_id);
    }

    return g_real_post_event_name_w(
        event_name,
        game_object_id,
        callback_mask,
        callback,
        cookie,
        external_source_count,
        external_sources,
        playing_id);
}

static AKRESULT __cdecl hook_set_state_a(const char *group_name, const char *state_name)
{
    char group_text[160];
    char state_text[160];

    if (copy_ascii_text(group_name, group_text, sizeof(group_text)) != 0 &&
        copy_ascii_text(state_name, state_text, sizeof(state_text)) != 0) {
        trace_pair_once("SetStateA", group_text, state_text, 0);
    }

    return g_real_set_state_a(group_name, state_name);
}

static AKRESULT __cdecl hook_set_state_w(const wchar_t *group_name, const wchar_t *state_name)
{
    char group_text[160];
    char state_text[160];

    if (copy_ascii_wide_text(group_name, group_text, sizeof(group_text)) != 0 &&
        copy_ascii_wide_text(state_name, state_text, sizeof(state_text)) != 0) {
        trace_pair_once("SetStateW", group_text, state_text, 0);
    }

    return g_real_set_state_w(group_name, state_name);
}

static AKRESULT __cdecl hook_set_state_id(AkUniqueID group_id, AkUniqueID state_id)
{
    trace_numeric_once("SetStateID", group_id, state_id, 0, 0.0f);
    return g_real_set_state_id(group_id, state_id);
}

static AKRESULT __cdecl hook_set_switch_a(const char *group_name, const char *state_name, AkGameObjectID game_object_id)
{
    char group_text[160];
    char state_text[160];

    if (copy_ascii_text(group_name, group_text, sizeof(group_text)) != 0 &&
        copy_ascii_text(state_name, state_text, sizeof(state_text)) != 0) {
        trace_pair_once("SetSwitchA", group_text, state_text, game_object_id);
    }

    return g_real_set_switch_a(group_name, state_name, game_object_id);
}

static AKRESULT __cdecl hook_set_switch_w(const wchar_t *group_name, const wchar_t *state_name, AkGameObjectID game_object_id)
{
    char group_text[160];
    char state_text[160];

    if (copy_ascii_wide_text(group_name, group_text, sizeof(group_text)) != 0 &&
        copy_ascii_wide_text(state_name, state_text, sizeof(state_text)) != 0) {
        trace_pair_once("SetSwitchW", group_text, state_text, game_object_id);
    }

    return g_real_set_switch_w(group_name, state_name, game_object_id);
}

static AKRESULT __cdecl hook_set_switch_id(AkUniqueID group_id, AkUniqueID state_id, AkGameObjectID game_object_id)
{
    trace_numeric_once("SetSwitchID", group_id, state_id, game_object_id, 0.0f);
    return g_real_set_switch_id(group_id, state_id, game_object_id);
}

static AKRESULT __cdecl hook_post_trigger_a(const char *trigger_name, AkGameObjectID game_object_id)
{
    char text[160];

    if (copy_ascii_text(trigger_name, text, sizeof(text)) != 0) {
        trace_single_once("PostTriggerA", text, game_object_id);
    }

    return g_real_post_trigger_a(trigger_name, game_object_id);
}

static AKRESULT __cdecl hook_post_trigger_w(const wchar_t *trigger_name, AkGameObjectID game_object_id)
{
    char text[160];

    if (copy_ascii_wide_text(trigger_name, text, sizeof(text)) != 0) {
        trace_single_once("PostTriggerW", text, game_object_id);
    }

    return g_real_post_trigger_w(trigger_name, game_object_id);
}

static AKRESULT __cdecl hook_post_trigger_id(AkUniqueID trigger_id, AkGameObjectID game_object_id)
{
    trace_numeric_once("PostTriggerID", trigger_id, 0, game_object_id, 0.0f);
    return g_real_post_trigger_id(trigger_id, game_object_id);
}

static AKRESULT __cdecl hook_set_rtpc_value_a(const char *rtpc_name, float value, AkGameObjectID game_object_id, int32_t interpolation_time_ms, int32_t fade_curve, BOOL bypass_game_param)
{
    char text[160];
    (void)interpolation_time_ms;
    (void)fade_curve;
    (void)bypass_game_param;

    if (copy_ascii_text(rtpc_name, text, sizeof(text)) != 0) {
        trace_single_once("SetRTPCA", text, game_object_id);
        if (reserve_trace_key(hash_trace_key("SetRTPCAValue", text, NULL))) {
            log_verbose("Trace SetRTPCAValue name=%s gameObject=0x%llx value=%.3f", text, (unsigned long long)game_object_id, (double)value);
        }
    }

    return g_real_set_rtpc_value_a(rtpc_name, value, game_object_id, interpolation_time_ms, fade_curve, bypass_game_param);
}

static AKRESULT __cdecl hook_set_rtpc_value_w(const wchar_t *rtpc_name, float value, AkGameObjectID game_object_id, int32_t interpolation_time_ms, int32_t fade_curve, BOOL bypass_game_param)
{
    char text[160];
    (void)interpolation_time_ms;
    (void)fade_curve;
    (void)bypass_game_param;

    if (copy_ascii_wide_text(rtpc_name, text, sizeof(text)) != 0) {
        trace_single_once("SetRTPCW", text, game_object_id);
        if (reserve_trace_key(hash_trace_key("SetRTPCWValue", text, NULL))) {
            log_verbose("Trace SetRTPCWValue name=%s gameObject=0x%llx value=%.3f", text, (unsigned long long)game_object_id, (double)value);
        }
    }

    return g_real_set_rtpc_value_w(rtpc_name, value, game_object_id, interpolation_time_ms, fade_curve, bypass_game_param);
}

static AKRESULT __cdecl hook_set_rtpc_value_id(AkUniqueID rtpc_id, float value, AkGameObjectID game_object_id, int32_t interpolation_time_ms, int32_t fade_curve, BOOL bypass_game_param)
{
    (void)interpolation_time_ms;
    (void)fade_curve;
    (void)bypass_game_param;
    trace_numeric_once("SetRTPCID", rtpc_id, 0, game_object_id, value);
    return g_real_set_rtpc_value_id(rtpc_id, value, game_object_id, interpolation_time_ms, fade_curve, bypass_game_param);
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
        "DollmanMute init start: enabled=%d verbose=%d playerVoiceHooks=%d markHotkey=%d vk=%u scannerMode=%u",
        g_cfg.enabled,
        g_cfg.verbose_log,
        g_cfg.enable_player_voice_hooks,
        g_cfg.enable_mark_hotkey,
        (unsigned int)g_cfg.mark_hotkey_virtual_key,
        (unsigned int)g_cfg.scanner_mode);

    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("MH_Initialize failed: %d", (int)status);
        return 0;
    }

    if (install_export_hook(k_export_post_event_id, hook_post_event_id, (void **)&g_real_post_event_id, "PostEventID")) {
        ++hook_count;
    }
    if (g_cfg.verbose_log) {
        if (install_export_hook(k_export_get_id_from_string_a, hook_get_id_from_string_a, (void **)&g_real_get_id_from_string_a, "GetIDFromStringA")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_get_id_from_string_w, hook_get_id_from_string_w, (void **)&g_real_get_id_from_string_w, "GetIDFromStringW")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_post_event_name_a, hook_post_event_name_a, (void **)&g_real_post_event_name_a, "PostEventA")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_post_event_name_w, hook_post_event_name_w, (void **)&g_real_post_event_name_w, "PostEventW")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_state_a, hook_set_state_a, (void **)&g_real_set_state_a, "SetStateA")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_state_w, hook_set_state_w, (void **)&g_real_set_state_w, "SetStateW")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_state_id, hook_set_state_id, (void **)&g_real_set_state_id, "SetStateID")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_switch_a, hook_set_switch_a, (void **)&g_real_set_switch_a, "SetSwitchA")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_switch_w, hook_set_switch_w, (void **)&g_real_set_switch_w, "SetSwitchW")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_switch_id, hook_set_switch_id, (void **)&g_real_set_switch_id, "SetSwitchID")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_post_trigger_a, hook_post_trigger_a, (void **)&g_real_post_trigger_a, "PostTriggerA")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_post_trigger_w, hook_post_trigger_w, (void **)&g_real_post_trigger_w, "PostTriggerW")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_post_trigger_id, hook_post_trigger_id, (void **)&g_real_post_trigger_id, "PostTriggerID")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_rtpc_value_a, hook_set_rtpc_value_a, (void **)&g_real_set_rtpc_value_a, "SetRTPCA")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_rtpc_value_w, hook_set_rtpc_value_w, (void **)&g_real_set_rtpc_value_w, "SetRTPCW")) {
            ++hook_count;
        }
        if (install_export_hook(k_export_set_rtpc_value_id, hook_set_rtpc_value_id, (void **)&g_real_set_rtpc_value_id, "SetRTPCID")) {
            ++hook_count;
        }
    } else {
        log_line("Wwise name trace hooks disabled because VerboseLog=0");
    }
    if (g_cfg.enable_player_voice_hooks) {
        if (install_rva_hook(k_rva_play_voice_impl, hook_play_voice_impl, (void **)&g_real_play_voice_impl, "PlayVoiceImpl")) {
            ++hook_count;
        }
        if (install_rva_hook(k_rva_play_voice_with_sentence_impl, hook_play_voice_with_sentence_impl, (void **)&g_real_play_voice_with_sentence_impl, "PlayVoiceWithSentenceImpl")) {
            ++hook_count;
        }
        if (install_rva_hook(k_rva_play_voice_with_sentence_randomly_impl, hook_play_voice_with_sentence_randomly_impl, (void **)&g_real_play_voice_with_sentence_randomly_impl, "PlayVoiceWithSentenceRandomlyImpl")) {
            ++hook_count;
        }
    } else {
        log_line("PlayerVoice hooks disabled by config; using PostEventID compatibility mode");
    }

    g_hooks_installed = hook_count != 0;
    log_line("DollmanMute init complete: hooks=%u", hook_count);

    if (g_cfg.enable_mark_hotkey && g_cfg.mark_hotkey_virtual_key != 0 && !g_mark_hotkey_thread_running) {
        HANDLE mark_thread = CreateThread(
            NULL,
            0,
            mark_hotkey_thread_proc,
            (LPVOID)(uintptr_t)g_cfg.mark_hotkey_virtual_key,
            0,
            NULL);
        if (mark_thread != NULL) {
            CloseHandle(mark_thread);
        } else {
            log_line("Failed to start mark hotkey thread");
        }
    }

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
    InitializeCriticalSection(&g_trace_lock);
    DisableThreadLibraryCalls(instance);

    thread_handle = CreateThread(NULL, 0, initialize_thread_proc, NULL, 0, NULL);
    if (thread_handle != NULL) {
        CloseHandle(thread_handle);
    }

    return TRUE;
}

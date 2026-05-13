/*
 * DollmanMute core, dynamic-resolution branch.
 *
 * Goal: gameplay-only Dollman voice + subtitle mute on DS2 v1.7.76, with
 * minimum hardcoded dependencies on the live binary's RVAs. Class vtables and
 * the function pointers reachable through them are resolved at runtime via
 * MSVC RTTI, so a future patch that shuffles RVAs but keeps class names will
 * not silently miss its targets. The handful of internal functions still
 * reached through fixed RVAs are sanity-checked against the resolved .text
 * range so the hook set disables itself when the addresses look wrong instead
 * of binding to garbage.
 *
 * Mute architecture:
 *   1. Hook DSRadioSentenceGroupThroughDollmanInstance::vtable[2] (the shared
 *      voice-delay dispatcher). On entry, if the instance vtable matches the
 *      Dollman radio class, mark a TLS flag.
 *   2. Hook DSTalkManagerImpl_QueueStartTalkFunction. On exit, if the TLS flag
 *      from step 1 is set, set bit 1 of the new StartTalkFunction's +0x68 byte
 *      and store the StartTalkFunction pointer in a small set. Engine itself
 *      reads +0x68 bit 1 in StartTalkFunction_UpdateAdvance and skips subtitle
 *      sender context build naturally. No subtitle-side hook required.
 *   3. Hook StartTalkFunction::vtable[15] (UpdateAdvance). On entry, if `this`
 *      is in the set or has +0x68 bit 1 set, mark a per-call TLS flag.
 *   4. Hook TalkSound_CreateSoundInstanceWrapper. On entry, if the per-call
 *      TLS flag is set, write *out=0 and return without invoking the original.
 *      No wrapper means no Wwise sound instance and no PostEvent.
 *
 * Both subtitle suppression (via engine's natural +0x68 gate) and voice
 * suppression (via wrapper substitution) leave the StartTalkFunction's other
 * lifecycle work intact, so private-room/story scene completion remains safe.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "MinHook.h"
#include "core_api.h"

/* ------------------------------------------------------------------------- */
/* Build / target info                                                        */
/* ------------------------------------------------------------------------- */

static const char *k_build_tag = "v3.0-dynamic-resolution-dev";

/*
 * Hardcoded v1.7 RVAs for the two internal functions not yet resolved
 * dynamically. They are sanity-checked against the live .text range and a
 * known function-prefix byte pattern before use; TODO: replace with string-xref
 * or pattern signatures so future builds are also supported without per-version
 * constants.
 */
static const uintptr_t k_v17_rva_queue_start_talk_function = 0x00388C20u;
static const uintptr_t k_v17_rva_echoback_enqueue = 0x00389200u;
static const uintptr_t k_v17_rva_talk_sound_create_wrapper = 0x003899C0u;
static const uintptr_t k_v17_rva_dollman_voice_closure = 0x00C743B0u;
static const uintptr_t k_v17_rva_voice_shared_helper = 0x00DACCD0u;
static const uint8_t k_v17_queue_start_talk_prefix[] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x89,
    0x6c, 0x24, 0x50, 0x48, 0x8d, 0x59, 0x10, 0x48
};
static const uint8_t k_v17_echoback_enqueue_prefix[] = {
    0x48, 0x89, 0x74, 0x24, 0x18, 0x41, 0x56, 0x48,
    0x83, 0xec, 0x20, 0x48, 0x8b, 0x35
};
static const uint8_t k_v17_talk_sound_wrapper_prefix[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
    0x24, 0x18, 0x56, 0x48, 0x83, 0xec, 0x20, 0x33
};
static const uint8_t k_v17_dollman_voice_closure_prefix[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x48, 0x8b,
    0xec, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00
};
static const uint8_t k_v17_voice_shared_helper_prefix[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55
};

/* ------------------------------------------------------------------------- */
/* Globals                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct Config {
    BOOL enabled;
    BOOL verbose_log;
    BOOL enable_voice_mute;
    BOOL enable_subtitle_mute;
    BOOL hook_radio_dispatcher;
    BOOL hook_echoback;
    BOOL hook_queue_starttalk;
    BOOL hook_starttalk_update;
    BOOL hook_talksound_wrapper;
    BOOL hook_dollman_voice_schedule;
    BOOL hook_dollman_voice_closure;
    BOOL hook_voice_shared_helper;
    int toggle_hotkey_vk;
} Config;

static HMODULE g_self_module = NULL;
static CRITICAL_SECTION g_log_lock;
static BOOL g_log_lock_inited = FALSE;
static Config g_cfg;
static char g_ini_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static ProxyContext g_proxy_ctx;
static volatile LONG g_core_shutting_down = 0;
static volatile LONG g_runtime_enabled = 1;
static volatile LONG g_hooks_runtime_enabled = 0;
static HANDLE g_hotkey_thread = NULL;

static uintptr_t g_image_base = 0;
static uintptr_t g_image_size = 0;
static uintptr_t g_text_start = 0;
static uintptr_t g_text_end = 0;
static uintptr_t g_rdata_start = 0;
static uintptr_t g_rdata_end = 0;

/*
 * MSVC RTTI type descriptors live in a writable data section (named ".data"
 * in DS2.exe). Vtables and complete-object locators live in .rdata. We track
 * up to four data ranges since the v1.7 PE has two .data sections split by an
 * .idata stub between them.
 */
#define MAX_DATA_RANGES 8
typedef struct DataRange { uintptr_t start; uintptr_t end; } DataRange;
static DataRange g_data_ranges[MAX_DATA_RANGES];
static int g_data_range_count = 0;

static uint32_t  g_ds2_timedatestamp = 0;

/* TLS slot indices */
static DWORD g_tls_radio_is_dollman = TLS_OUT_OF_INDEXES;
static DWORD g_tls_current_starttalk_is_dollman = TLS_OUT_OF_INDEXES;

#define DOLLMAN_ECHOBACK_MAX 128
#define DOLLMAN_STARTTALK_MAX 128
#define DOLLMAN_ECHOBACK_TTL_MS 30000ull
#define DOLLMAN_STARTTALK_TTL_MS 120000ull
static CRITICAL_SECTION g_state_lock;
static BOOL g_state_lock_inited = FALSE;
static uintptr_t g_dollman_echoback[DOLLMAN_ECHOBACK_MAX];
static ULONGLONG g_dollman_echoback_expiry[DOLLMAN_ECHOBACK_MAX];
static volatile uintptr_t g_dollman_starttalk[DOLLMAN_STARTTALK_MAX];
static volatile ULONGLONG g_dollman_starttalk_expiry[DOLLMAN_STARTTALK_MAX];
static size_t g_dollman_echoback_next = 0;
static size_t g_dollman_starttalk_next = 0;

/* ------------------------------------------------------------------------- */
/* Logging                                                                    */
/* ------------------------------------------------------------------------- */

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
        line, sizeof(line),
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        message);
    if (line_len <= 0) {
        return;
    }
    if (g_log_lock_inited) {
        EnterCriticalSection(&g_log_lock);
    }
    file = CreateFileA(
        g_log_path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, (DWORD)line_len, &written, NULL);
        CloseHandle(file);
    }
    if (g_log_lock_inited) {
        LeaveCriticalSection(&g_log_lock);
    }
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

/* ------------------------------------------------------------------------- */
/* Config                                                                     */
/* ------------------------------------------------------------------------- */

static const char *k_default_ini =
    "; DollmanMute v3 dynamic-resolution config.\n"
    "; This build only supports DS2 v1.7.76. Other versions are detected at\n"
    "; init and the hook set is left disabled rather than binding to wrong\n"
    "; addresses. Check DollmanMute.log for the resolved hook list.\n"
    "[General]\n"
    "Enabled=1\n"
    "VerboseLog=0\n"
    "EnableVoiceMute=1\n"
    "EnableSubtitleMute=1\n"
    "HookRadioDispatcher=1\n"
    "HookEchoback=0\n"
    "HookQueueStartTalk=0\n"
    "HookStartTalkUpdate=0\n"
    "HookTalkSoundWrapper=0\n"
    "HookDollmanVoiceSchedule=1\n"
    "HookDollmanVoiceClosure=1\n"
    "HookVoiceSharedHelper=1\n"
    "ToggleHotkeyVK=119\n";

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

    g_ini_path[0] = '\0';
    g_log_path[0] = '\0';
    if (g_proxy_ctx.ini_path != NULL && g_proxy_ctx.ini_path[0] != '\0') {
        snprintf(g_ini_path, sizeof(g_ini_path), "%s", g_proxy_ctx.ini_path);
    }
    if (g_proxy_ctx.log_path != NULL && g_proxy_ctx.log_path[0] != '\0') {
        snprintf(g_log_path, sizeof(g_log_path), "%s", g_proxy_ctx.log_path);
    }
    if (g_ini_path[0] != '\0' && g_log_path[0] != '\0') {
        return;
    }
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
    if (g_ini_path[0] == '\0') {
        join_path(g_ini_path, sizeof(g_ini_path), module_path, "DollmanMute.ini");
    }
    if (g_log_path[0] == '\0') {
        join_path(g_log_path, sizeof(g_log_path), module_path, "DollmanMute.log");
    }
}

static void ensure_default_ini(void)
{
    HANDLE file;
    DWORD written = 0;
    if (g_ini_path[0] == '\0' ||
        GetFileAttributesA(g_ini_path) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    file = CreateFileA(
        g_ini_path, GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(file, k_default_ini, (DWORD)strlen(k_default_ini), &written, NULL);
    CloseHandle(file);
}

static BOOL read_ini_bool(const char *section, const char *key, BOOL default_value)
{
    int value = GetPrivateProfileIntA(section, key, default_value ? 1 : 0, g_ini_path);
    return value != 0;
}

static int read_ini_int(const char *section, const char *key, int default_value)
{
    return GetPrivateProfileIntA(section, key, default_value, g_ini_path);
}

static void load_config(void)
{
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    g_cfg.enabled = TRUE;
    g_cfg.verbose_log = FALSE;
    g_cfg.enable_voice_mute = TRUE;
    g_cfg.enable_subtitle_mute = TRUE;
    g_cfg.hook_radio_dispatcher = TRUE;
    g_cfg.hook_echoback = FALSE;
    g_cfg.hook_queue_starttalk = FALSE;
    g_cfg.hook_starttalk_update = FALSE;
    g_cfg.hook_talksound_wrapper = FALSE;
    g_cfg.hook_dollman_voice_schedule = TRUE;
    g_cfg.hook_dollman_voice_closure = TRUE;
    g_cfg.hook_voice_shared_helper = TRUE;
    g_cfg.toggle_hotkey_vk = VK_F8;
    ensure_default_ini();
    g_cfg.enabled = read_ini_bool("General", "Enabled", g_cfg.enabled);
    g_cfg.verbose_log = read_ini_bool("General", "VerboseLog", g_cfg.verbose_log);
    g_cfg.enable_voice_mute = read_ini_bool("General", "EnableVoiceMute", g_cfg.enable_voice_mute);
    g_cfg.enable_subtitle_mute = read_ini_bool("General", "EnableSubtitleMute", g_cfg.enable_subtitle_mute);
    g_cfg.hook_radio_dispatcher = read_ini_bool("General", "HookRadioDispatcher", g_cfg.hook_radio_dispatcher);
    g_cfg.hook_echoback = read_ini_bool("General", "HookEchoback", g_cfg.hook_echoback);
    g_cfg.hook_queue_starttalk = read_ini_bool("General", "HookQueueStartTalk", g_cfg.hook_queue_starttalk);
    g_cfg.hook_starttalk_update = read_ini_bool("General", "HookStartTalkUpdate", g_cfg.hook_starttalk_update);
    g_cfg.hook_talksound_wrapper = read_ini_bool("General", "HookTalkSoundWrapper", g_cfg.hook_talksound_wrapper);
    g_cfg.hook_dollman_voice_schedule = read_ini_bool("General", "HookDollmanVoiceSchedule", g_cfg.hook_dollman_voice_schedule);
    g_cfg.hook_dollman_voice_closure = read_ini_bool("General", "HookDollmanVoiceClosure", g_cfg.hook_dollman_voice_closure);
    g_cfg.hook_voice_shared_helper = read_ini_bool("General", "HookVoiceSharedHelper", g_cfg.hook_voice_shared_helper);
    g_cfg.toggle_hotkey_vk = read_ini_int("General", "ToggleHotkeyVK", g_cfg.toggle_hotkey_vk);
}

/* ------------------------------------------------------------------------- */
/* PE introspection: image base, sections, TimeDateStamp                      */
/* ------------------------------------------------------------------------- */

static uintptr_t safe_read_ptr(uintptr_t addr)
{
    if (addr == 0 || IsBadReadPtr((const void *)addr, sizeof(uintptr_t))) {
        return 0;
    }
    return *(uintptr_t *)addr;
}

static uint32_t safe_read_u32(uintptr_t addr)
{
    if (addr == 0 || IsBadReadPtr((const void *)addr, sizeof(uint32_t))) {
        return 0;
    }
    return *(uint32_t *)addr;
}

static BOOL bytes_match(uintptr_t addr, const uint8_t *expected, size_t size)
{
    if (addr == 0 || IsBadReadPtr((const void *)addr, size)) {
        return FALSE;
    }
    return memcmp((const void *)addr, expected, size) == 0;
}

static BOOL pe_introspect(void)
{
    HMODULE mod = GetModuleHandleW(L"DS2.exe");
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER section;
    int i;

    if (mod == NULL) {
        log_line("PE introspect failed: GetModuleHandleW(DS2.exe)=NULL");
        return FALSE;
    }
    g_image_base = (uintptr_t)mod;
    dos = (PIMAGE_DOS_HEADER)g_image_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log_line("PE introspect failed: DOS signature mismatch");
        return FALSE;
    }
    nt = (PIMAGE_NT_HEADERS)(g_image_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        log_line("PE introspect failed: NT signature mismatch");
        return FALSE;
    }
    g_image_size = nt->OptionalHeader.SizeOfImage;
    g_ds2_timedatestamp = nt->FileHeader.TimeDateStamp;

    section = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uintptr_t start = g_image_base + section[i].VirtualAddress;
        uintptr_t size = section[i].Misc.VirtualSize;
        if (memcmp(section[i].Name, ".text", 6) == 0) {
            g_text_start = start;
            g_text_end = start + size;
        } else if (memcmp(section[i].Name, ".rdata", 7) == 0) {
            /* DS2.exe has a single .rdata section; if there are multiple,
             * the first one wins which is fine for COL/vtable scanning. */
            if (g_rdata_start == 0) {
                g_rdata_start = start;
                g_rdata_end = start + size;
            }
        } else if (memcmp(section[i].Name, ".data", 6) == 0) {
            if (g_data_range_count < MAX_DATA_RANGES) {
                g_data_ranges[g_data_range_count].start = start;
                g_data_ranges[g_data_range_count].end = start + size;
                g_data_range_count++;
            }
        }
    }

    log_line(
        "PE: base=0x%llx size=0x%llx tds=0x%08x",
        (unsigned long long)g_image_base,
        (unsigned long long)g_image_size,
        (unsigned)g_ds2_timedatestamp);
    log_line(
        "PE sections: .text=[0x%llx,0x%llx) .rdata=[0x%llx,0x%llx) data_ranges=%d",
        (unsigned long long)g_text_start, (unsigned long long)g_text_end,
        (unsigned long long)g_rdata_start, (unsigned long long)g_rdata_end,
        g_data_range_count);
    for (i = 0; i < g_data_range_count; i++) {
        log_line(
            "  .data[%d]=[0x%llx,0x%llx)",
            i,
            (unsigned long long)g_data_ranges[i].start,
            (unsigned long long)g_data_ranges[i].end);
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* MSVC RTTI scanner: class name -> vtable                                    */
/*                                                                            */
/* Layout, x64 image-relative addressing:                                     */
/*   TypeDescriptor (RTD): pTypeInfoVtbl@+0, spare@+8, name@+0x10 (0-term).   */
/*   CompleteObjectLocator (COL):                                             */
/*     +0x00 sig (0|1), +0x04 offset, +0x08 cdOff,                            */
/*     +0x0C TD RVA, +0x10 CHD RVA, +0x14 self RVA.                           */
/*   Vtable[-1] = pointer to COL.                                             */
/*                                                                            */
/* Scanner: locate name in .rdata, derive RTD = name - 0x10, then scan .rdata */
/* for COL (sig<=1, td_rva matches, self_rva matches), then scan .rdata for   */
/* the qword whose [-1] equals that COL.                                      */
/* ------------------------------------------------------------------------- */

static uintptr_t rtti_find_name_string_in_range(const char *name, size_t nlen, uintptr_t start, uintptr_t end)
{
    uintptr_t p, scan_end;
    if (start == 0 || end <= start || end - start <= nlen + 1) {
        return 0;
    }
    scan_end = end - nlen - 1;
    for (p = start; p < scan_end; p++) {
        const char *s = (const char *)p;
        if (s[0] != name[0]) {
            continue;
        }
        if (memcmp(s, name, nlen) == 0 && s[nlen] == '\0') {
            return p;
        }
    }
    return 0;
}

static uintptr_t rtti_find_name_string(const char *name)
{
    /*
     * MSVC TypeDescriptors live in writable data. Scan every tracked .data
     * range until we find an exact match for `name` followed by a NUL byte.
     */
    size_t nlen = strlen(name);
    int i;
    if (nlen == 0) {
        return 0;
    }
    for (i = 0; i < g_data_range_count; i++) {
        uintptr_t hit = rtti_find_name_string_in_range(
            name, nlen, g_data_ranges[i].start, g_data_ranges[i].end);
        if (hit != 0) {
            return hit;
        }
    }
    /* Fallback: also try .rdata in case future builds move TDs there. */
    return rtti_find_name_string_in_range(name, nlen, g_rdata_start, g_rdata_end);
}

static uintptr_t rtti_find_col_for_td(uintptr_t td)
{
    uintptr_t p, end;
    uint32_t td_rva;
    if (td == 0 || g_rdata_start == 0) {
        return 0;
    }
    td_rva = (uint32_t)(td - g_image_base);
    end = g_rdata_end - 0x18;
    for (p = g_rdata_start; p < end; p += 4) {
        uint32_t sig = *(uint32_t *)p;
        uint32_t rva_td;
        uint32_t rva_self;
        if (sig > 1) {
            continue;
        }
        rva_td = *(uint32_t *)(p + 0x0C);
        if (rva_td != td_rva) {
            continue;
        }
        rva_self = *(uint32_t *)(p + 0x14);
        if (rva_self != (uint32_t)(p - g_image_base)) {
            continue;
        }
        return p;
    }
    return 0;
}

static uintptr_t rtti_find_vtable_for_col(uintptr_t col)
{
    uintptr_t p, end;
    if (col == 0 || g_rdata_start == 0) {
        return 0;
    }
    end = g_rdata_end - 8;
    for (p = g_rdata_start + 8; p < end; p += 8) {
        uintptr_t prev = *(uintptr_t *)(p - 8);
        uintptr_t v0;
        if (prev != col) {
            continue;
        }
        v0 = *(uintptr_t *)p;
        if (v0 < g_text_start || v0 >= g_text_end) {
            continue;
        }
        return p;
    }
    return 0;
}

static uintptr_t rtti_resolve_vtable(const char *raw_rtti_name)
{
    /*
     * `raw_rtti_name` is the body of the RTTI name string as it lives in the
     * binary, e.g. ".?AVDSRadioSentenceGroupThroughDollmanInstance@@". Caller
     * passes the exact bytes that follow the type-info vtable pointer in the
     * MSVC type descriptor.
     */
    uintptr_t name = rtti_find_name_string(raw_rtti_name);
    uintptr_t td;
    uintptr_t col;
    uintptr_t vtbl;
    if (name == 0) {
        return 0;
    }
    td = name - 0x10;
    col = rtti_find_col_for_td(td);
    if (col == 0) {
        return 0;
    }
    vtbl = rtti_find_vtable_for_col(col);
    return vtbl;
}

/* ------------------------------------------------------------------------- */
/* Resolved targets registry                                                  */
/* ------------------------------------------------------------------------- */

typedef struct ResolvedTargets {
    uintptr_t vtbl_dollman_instance;
    uintptr_t vtbl_player_instance;
    uintptr_t vtbl_starttalk_function;
    uintptr_t vtbl_echoback_function;
    uintptr_t vtbl_dummy_wrapper;
    uintptr_t vtbl_simple_wrapper;
    uintptr_t vtbl_graph_wrapper;

    uintptr_t fn_radio_voice_dispatcher; /* DollmanInstance.vtable[2], shared with Player */
    uintptr_t fn_dollman_voice_schedule; /* DollmanInstance.vtable[8], probe only */
    uintptr_t fn_echoback_enqueue; /* hardcoded v1.7, pending dynamic resolver */
    uintptr_t fn_echoback_execute; /* EchobackFunction.vtable[2] */
    uintptr_t fn_starttalk_update_advance; /* StartTalkFunction.vtable[15] */
    uintptr_t fn_queue_starttalk_function; /* hardcoded v1.7, pending dynamic resolver */
    uintptr_t fn_talksound_create_wrapper; /* hardcoded v1.7, pending dynamic resolver */
    uintptr_t fn_dollman_voice_closure; /* hardcoded v1.7 Dollman delay closure body */
    uintptr_t fn_voice_shared_helper; /* hardcoded v1.7 shared voice queue helper */
    uintptr_t ptr_dstalk_manager_global; /* RIP-relative global used by echoback enqueue */
} ResolvedTargets;

static ResolvedTargets g_targets;

static uintptr_t vtable_slot(uintptr_t vtbl, int slot)
{
    if (vtbl == 0) {
        return 0;
    }
    return safe_read_ptr(vtbl + (uintptr_t)slot * 8u);
}

static BOOL is_in_text_range(uintptr_t addr)
{
    return addr != 0 && addr >= g_text_start && addr < g_text_end;
}

static BOOL is_text_range(uintptr_t addr, size_t size)
{
    return addr != 0 &&
           size != 0 &&
           addr >= g_text_start &&
           addr + size >= addr &&
           addr + size <= g_text_end;
}

static BOOL fixed_fallback_bytes_match(
    uintptr_t queue,
    uintptr_t echoback_enqueue,
    uintptr_t wrapper,
    uintptr_t dollman_voice_closure,
    uintptr_t voice_shared_helper)
{
    if (!is_text_range(queue, sizeof(k_v17_queue_start_talk_prefix)) ||
        !is_text_range(echoback_enqueue, sizeof(k_v17_echoback_enqueue_prefix)) ||
        !is_text_range(wrapper, sizeof(k_v17_talk_sound_wrapper_prefix)) ||
        !is_text_range(dollman_voice_closure, sizeof(k_v17_dollman_voice_closure_prefix)) ||
        !is_text_range(voice_shared_helper, sizeof(k_v17_voice_shared_helper_prefix))) {
        log_line("v1.7 fallback byte check failed: target outside .text");
        return FALSE;
    }
    if (!bytes_match(queue, k_v17_queue_start_talk_prefix, sizeof(k_v17_queue_start_talk_prefix))) {
        log_line("v1.7 fallback byte check failed: QueueStartTalkFunction prefix mismatch");
        return FALSE;
    }
    if (!bytes_match(echoback_enqueue, k_v17_echoback_enqueue_prefix, sizeof(k_v17_echoback_enqueue_prefix))) {
        log_line("v1.7 fallback byte check failed: Echoback enqueue prefix mismatch");
        return FALSE;
    }
    if (!bytes_match(wrapper, k_v17_talk_sound_wrapper_prefix, sizeof(k_v17_talk_sound_wrapper_prefix))) {
        log_line("v1.7 fallback byte check failed: TalkSound_CreateSoundInstanceWrapper prefix mismatch");
        return FALSE;
    }
    if (!bytes_match(dollman_voice_closure, k_v17_dollman_voice_closure_prefix, sizeof(k_v17_dollman_voice_closure_prefix))) {
        log_line("v1.7 fallback byte check failed: Dollman voice closure prefix mismatch");
        return FALSE;
    }
    if (!bytes_match(voice_shared_helper, k_v17_voice_shared_helper_prefix, sizeof(k_v17_voice_shared_helper_prefix))) {
        log_line("v1.7 fallback byte check failed: VoiceSharedHelper prefix mismatch");
        return FALSE;
    }
    log_line("v1.7 fallback byte check passed for TimeDateStamp 0x%08x", (unsigned)g_ds2_timedatestamp);
    return TRUE;
}

static BOOL resolve_all(void)
{
    g_targets.vtbl_dollman_instance =
        rtti_resolve_vtable(".?AVDSRadioSentenceGroupThroughDollmanInstance@@");
    g_targets.vtbl_player_instance =
        rtti_resolve_vtable(".?AVDSRadioSentenceGroupThroughPlayerInstance@@");
    g_targets.vtbl_starttalk_function =
        rtti_resolve_vtable(".?AVStartTalkFunction@DSTalkInternal@@");
    g_targets.vtbl_echoback_function =
        rtti_resolve_vtable(".?AVEchobackFunction@DSTalkInternal@@");
    g_targets.vtbl_dummy_wrapper =
        rtti_resolve_vtable(".?AVDummySoundInstanceWrapper@DSTalkInternal@@");
    g_targets.vtbl_simple_wrapper =
        rtti_resolve_vtable(".?AVSimpleSoundInstanceWrapper@DSTalkInternal@@");
    g_targets.vtbl_graph_wrapper =
        rtti_resolve_vtable(".?AVGraphSoundInstanceWrapper@DSTalkInternal@@");

    log_line(
        "RTTI: dollman=0x%llx player=0x%llx starttalk=0x%llx echoback=0x%llx dummy=0x%llx simple=0x%llx graph=0x%llx",
        (unsigned long long)g_targets.vtbl_dollman_instance,
        (unsigned long long)g_targets.vtbl_player_instance,
        (unsigned long long)g_targets.vtbl_starttalk_function,
        (unsigned long long)g_targets.vtbl_echoback_function,
        (unsigned long long)g_targets.vtbl_dummy_wrapper,
        (unsigned long long)g_targets.vtbl_simple_wrapper,
        (unsigned long long)g_targets.vtbl_graph_wrapper);

    g_targets.fn_radio_voice_dispatcher =
        vtable_slot(g_targets.vtbl_dollman_instance, 2);
    g_targets.fn_dollman_voice_schedule =
        vtable_slot(g_targets.vtbl_dollman_instance, 8);
    g_targets.fn_echoback_execute =
        vtable_slot(g_targets.vtbl_echoback_function, 2);
    g_targets.fn_starttalk_update_advance =
        vtable_slot(g_targets.vtbl_starttalk_function, 15);

    if (g_ds2_timedatestamp != 0) {
        int32_t manager_disp;
        g_targets.fn_echoback_enqueue =
            g_image_base + k_v17_rva_echoback_enqueue;
        g_targets.fn_queue_starttalk_function =
            g_image_base + k_v17_rva_queue_start_talk_function;
        g_targets.fn_talksound_create_wrapper =
            g_image_base + k_v17_rva_talk_sound_create_wrapper;
        g_targets.fn_dollman_voice_closure =
            g_image_base + k_v17_rva_dollman_voice_closure;
        g_targets.fn_voice_shared_helper =
            g_image_base + k_v17_rva_voice_shared_helper;
        manager_disp = *(int32_t *)(g_targets.fn_echoback_enqueue + 14);
        g_targets.ptr_dstalk_manager_global =
            g_targets.fn_echoback_enqueue + 18 + (intptr_t)manager_disp;
    }

    log_line(
        "Resolved fns: radio_disp=0x%llx schedule=0x%llx echoback_enq=0x%llx echoback_exec=0x%llx update_adv=0x%llx queue=0x%llx wrap=0x%llx dollman_closure=0x%llx voice_helper=0x%llx manager_ptr=0x%llx",
        (unsigned long long)g_targets.fn_radio_voice_dispatcher,
        (unsigned long long)g_targets.fn_dollman_voice_schedule,
        (unsigned long long)g_targets.fn_echoback_enqueue,
        (unsigned long long)g_targets.fn_echoback_execute,
        (unsigned long long)g_targets.fn_starttalk_update_advance,
        (unsigned long long)g_targets.fn_queue_starttalk_function,
        (unsigned long long)g_targets.fn_talksound_create_wrapper,
        (unsigned long long)g_targets.fn_dollman_voice_closure,
        (unsigned long long)g_targets.fn_voice_shared_helper,
        (unsigned long long)g_targets.ptr_dstalk_manager_global);

    if (g_targets.vtbl_dollman_instance == 0 ||
        g_targets.vtbl_starttalk_function == 0 ||
        g_targets.vtbl_echoback_function == 0 ||
        g_targets.vtbl_dummy_wrapper == 0) {
        log_line("Resolver failed: required RTTI vtable missing");
        return FALSE;
    }
    if (!is_in_text_range(g_targets.fn_radio_voice_dispatcher) ||
        !is_in_text_range(g_targets.fn_echoback_enqueue) ||
        !is_in_text_range(g_targets.fn_echoback_execute) ||
        !is_in_text_range(g_targets.fn_starttalk_update_advance) ||
        !is_in_text_range(g_targets.fn_queue_starttalk_function) ||
        !is_in_text_range(g_targets.fn_talksound_create_wrapper) ||
        !is_in_text_range(g_targets.fn_dollman_voice_closure) ||
        !is_in_text_range(g_targets.fn_voice_shared_helper)) {
        log_line(
            "Resolver failed: function target outside .text range "
            "(radio_disp=0x%llx echoback_enq=0x%llx echoback_exec=0x%llx update_adv=0x%llx queue=0x%llx wrap=0x%llx dollman_closure=0x%llx voice_helper=0x%llx text=[0x%llx,0x%llx))",
            (unsigned long long)g_targets.fn_radio_voice_dispatcher,
            (unsigned long long)g_targets.fn_echoback_enqueue,
            (unsigned long long)g_targets.fn_echoback_execute,
            (unsigned long long)g_targets.fn_starttalk_update_advance,
            (unsigned long long)g_targets.fn_queue_starttalk_function,
            (unsigned long long)g_targets.fn_talksound_create_wrapper,
            (unsigned long long)g_targets.fn_dollman_voice_closure,
            (unsigned long long)g_targets.fn_voice_shared_helper,
            (unsigned long long)g_text_start,
            (unsigned long long)g_text_end);
        return FALSE;
    }
    if (!fixed_fallback_bytes_match(
            g_targets.fn_queue_starttalk_function,
            g_targets.fn_echoback_enqueue,
            g_targets.fn_talksound_create_wrapper,
            g_targets.fn_dollman_voice_closure,
            g_targets.fn_voice_shared_helper)) {
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* TLS helpers                                                                */
/* ------------------------------------------------------------------------- */

static BOOL tls_get_bool(DWORD slot)
{
    if (slot == TLS_OUT_OF_INDEXES) {
        return FALSE;
    }
    return TlsGetValue(slot) != NULL;
}

static void tls_set_bool(DWORD slot, BOOL value)
{
    if (slot == TLS_OUT_OF_INDEXES) {
        return;
    }
    TlsSetValue(slot, value ? (LPVOID)(uintptr_t)1 : NULL);
}

static BOOL runtime_enabled(void)
{
    return g_cfg.enabled && g_runtime_enabled != 0;
}

static void set_hooks_runtime_enabled(BOOL enabled)
{
    MH_STATUS status;
    LONG old_value = InterlockedCompareExchange(&g_hooks_runtime_enabled, 0, 0);
    if ((enabled && old_value != 0) || (!enabled && old_value == 0)) {
        return;
    }
    status = enabled ? MH_EnableHook(MH_ALL_HOOKS) : MH_DisableHook(MH_ALL_HOOKS);
    if (status == MH_OK) {
        InterlockedExchange(&g_hooks_runtime_enabled, enabled ? 1 : 0);
        log_line("Hook runtime toggle: hooks %s", enabled ? "enabled" : "disabled");
    } else {
        log_line(
            "Hook runtime toggle failed: target=%s status=%d",
            enabled ? "enabled" : "disabled",
            (int)status);
    }
}

static void clear_state_tracking(void)
{
    size_t i;
    if (!g_state_lock_inited) {
        return;
    }
    EnterCriticalSection(&g_state_lock);
    for (i = 0; i < DOLLMAN_ECHOBACK_MAX; i++) {
        g_dollman_echoback[i] = 0;
        g_dollman_echoback_expiry[i] = 0;
    }
    for (i = 0; i < DOLLMAN_STARTTALK_MAX; i++) {
        g_dollman_starttalk[i] = 0;
        g_dollman_starttalk_expiry[i] = 0;
    }
    g_dollman_echoback_next = 0;
    g_dollman_starttalk_next = 0;
    LeaveCriticalSection(&g_state_lock);
}

static void remember_dollman_echoback(uintptr_t ptr)
{
    size_t slot;
    if (ptr == 0 || !g_state_lock_inited) {
        return;
    }
    EnterCriticalSection(&g_state_lock);
    slot = g_dollman_echoback_next % DOLLMAN_ECHOBACK_MAX;
    g_dollman_echoback_expiry[slot] = GetTickCount64() + DOLLMAN_ECHOBACK_TTL_MS;
    g_dollman_echoback[slot] = ptr;
    g_dollman_echoback_next++;
    LeaveCriticalSection(&g_state_lock);
}

static void remember_dollman_starttalk(uintptr_t ptr)
{
    size_t slot;
    if (ptr == 0 || !g_state_lock_inited) {
        return;
    }
    EnterCriticalSection(&g_state_lock);
    slot = g_dollman_starttalk_next % DOLLMAN_STARTTALK_MAX;
    g_dollman_starttalk_expiry[slot] = GetTickCount64() + DOLLMAN_STARTTALK_TTL_MS;
    g_dollman_starttalk[slot] = ptr;
    g_dollman_starttalk_next++;
    LeaveCriticalSection(&g_state_lock);
}

static BOOL is_dollman_starttalk(uintptr_t ptr)
{
    size_t i;
    if (ptr == 0 || !g_state_lock_inited) {
        return FALSE;
    }
    for (i = 0; i < DOLLMAN_STARTTALK_MAX; i++) {
        if (g_dollman_starttalk[i] == ptr) {
            ULONGLONG expiry = g_dollman_starttalk_expiry[i];
            return expiry != 0 && GetTickCount64() <= expiry;
        }
    }
    return FALSE;
}

static BOOL take_dollman_echoback(uintptr_t ptr)
{
    size_t i;
    BOOL found = FALSE;
    if (ptr == 0 || !g_state_lock_inited) {
        return FALSE;
    }
    EnterCriticalSection(&g_state_lock);
    for (i = 0; i < DOLLMAN_ECHOBACK_MAX; i++) {
        if (g_dollman_echoback[i] == ptr) {
            ULONGLONG expiry = g_dollman_echoback_expiry[i];
            if (expiry != 0 && GetTickCount64() <= expiry) {
                found = TRUE;
            }
            g_dollman_echoback[i] = 0;
            g_dollman_echoback_expiry[i] = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_state_lock);
    return found;
}

/* ------------------------------------------------------------------------- */
/* Mute counters (diagnostic)                                                 */
/* ------------------------------------------------------------------------- */

static volatile LONG g_radio_dispatcher_dollman_hits = 0;
static volatile LONG g_echoback_marked_dollman = 0;
static volatile LONG g_echoback_executed_dollman = 0;
static volatile LONG g_queue_starttalk_seen = 0;
static volatile LONG g_queue_starttalk_tls_seen = 0;
static volatile LONG g_starttalk_marked_dollman = 0;
static volatile LONG g_voice_wrapper_substituted = 0;
static volatile LONG g_dollman_voice_schedule_seen = 0;
static volatile LONG g_dollman_voice_closure_seen = 0;
static volatile LONG g_voice_helper_consumed = 0;
static volatile LONG g_subtitle_engine_skipped = 0;

/* ------------------------------------------------------------------------- */
/* Hook function pointers (originals)                                         */
/* ------------------------------------------------------------------------- */

typedef void   (__fastcall *RadioVoiceDispatcherFn)(uintptr_t self, uintptr_t a2, char a3, unsigned int a4);
typedef void   (__fastcall *EchobackEnqueueFn)(uintptr_t a1, uintptr_t payload);
typedef void   (__fastcall *EchobackExecuteFn)(uintptr_t self);
typedef void   (__fastcall *QueueStartTalkFunctionFn)(uintptr_t impl, uintptr_t payload);
typedef uintptr_t (__fastcall *StartTalkUpdateAdvanceFn)(uintptr_t self, double delta_seconds);
typedef uintptr_t *(__fastcall *TalkSoundCreateWrapperFn)(uintptr_t *out, uintptr_t sound_instance);
typedef uintptr_t (__fastcall *DollmanVoiceScheduleFn)(uintptr_t self, int controller_index);
typedef void   (__fastcall *DollmanVoiceClosureFn)(uintptr_t closure_payload);
typedef uint8_t (__fastcall *VoiceSharedHelperFn)(
    uintptr_t helper,
    uintptr_t voice_controller,
    uintptr_t notification_queue,
    int output_type,
    unsigned int *event_id);

static RadioVoiceDispatcherFn   g_real_radio_voice_dispatcher = NULL;
static EchobackEnqueueFn        g_real_echoback_enqueue = NULL;
static EchobackExecuteFn        g_real_echoback_execute = NULL;
static QueueStartTalkFunctionFn g_real_queue_starttalk_function = NULL;
static StartTalkUpdateAdvanceFn g_real_starttalk_update_advance = NULL;
static TalkSoundCreateWrapperFn g_real_talksound_create_wrapper = NULL;
static DollmanVoiceScheduleFn   g_real_dollman_voice_schedule = NULL;
static DollmanVoiceClosureFn    g_real_dollman_voice_closure = NULL;
static VoiceSharedHelperFn      g_real_voice_shared_helper = NULL;

/* ------------------------------------------------------------------------- */
/* Hook implementations                                                       */
/* ------------------------------------------------------------------------- */

static void __fastcall hook_radio_voice_dispatcher(
    uintptr_t self, uintptr_t a2, char a3, unsigned int a4)
{
    BOOL is_dollman = FALSE;
    if (runtime_enabled()) {
        uintptr_t vtbl = safe_read_ptr(self);
        is_dollman = (vtbl == g_targets.vtbl_dollman_instance);
    }
    if (is_dollman) {
        InterlockedIncrement(&g_radio_dispatcher_dollman_hits);
        tls_set_bool(g_tls_radio_is_dollman, TRUE);
        log_verbose(
            "[radio-disp] dollman self=0x%llx a3=%d a4=%u",
            (unsigned long long)self, (int)a3, (unsigned)a4);
    }
    if (g_real_radio_voice_dispatcher != NULL) {
        g_real_radio_voice_dispatcher(self, a2, a3, a4);
    }
    if (is_dollman) {
        tls_set_bool(g_tls_radio_is_dollman, FALSE);
    }
}

static void __fastcall hook_echoback_enqueue(uintptr_t a1, uintptr_t payload)
{
    BOOL was_dollman = runtime_enabled() && tls_get_bool(g_tls_radio_is_dollman);
    if (g_real_echoback_enqueue != NULL) {
        g_real_echoback_enqueue(a1, payload);
    }
    if (was_dollman) {
        uintptr_t manager = safe_read_ptr(g_targets.ptr_dstalk_manager_global);
        uint32_t count = safe_read_u32(manager + 0x18);
        uintptr_t data = safe_read_ptr(manager + 0x20);
        uintptr_t echoback = 0;
        uintptr_t vtbl = 0;
        if (data != 0 && count != 0) {
            echoback = safe_read_ptr(data + (uintptr_t)(count - 1) * 8u);
            vtbl = safe_read_ptr(echoback);
        }
        if (echoback != 0 && vtbl == g_targets.vtbl_echoback_function) {
            remember_dollman_echoback(echoback);
            InterlockedIncrement(&g_echoback_marked_dollman);
            log_verbose(
                "[echoback-enqueue] marked dollman echoback=0x%llx manager=0x%llx count=%u payload=0x%llx",
                (unsigned long long)echoback,
                (unsigned long long)manager,
                (unsigned)count,
                (unsigned long long)payload);
        } else {
            log_verbose(
                "[echoback-enqueue] dollman context but last deferred is not EchobackFunction echoback=0x%llx vtbl=0x%llx manager=0x%llx count=%u payload=0x%llx",
                (unsigned long long)echoback,
                (unsigned long long)vtbl,
                (unsigned long long)manager,
                (unsigned)count,
                (unsigned long long)payload);
        }
    }
}

static void __fastcall hook_echoback_execute(uintptr_t self)
{
    BOOL is_dollman = runtime_enabled() && take_dollman_echoback(self);
    if (is_dollman) {
        InterlockedIncrement(&g_echoback_executed_dollman);
        tls_set_bool(g_tls_radio_is_dollman, TRUE);
        log_verbose("[echoback-exec] dollman echoback=0x%llx", (unsigned long long)self);
    }
    if (g_real_echoback_execute != NULL) {
        g_real_echoback_execute(self);
    }
    if (is_dollman) {
        tls_set_bool(g_tls_radio_is_dollman, FALSE);
    }
}

static void __fastcall hook_queue_starttalk_function(uintptr_t impl, uintptr_t payload)
{
    BOOL was_dollman = runtime_enabled() && tls_get_bool(g_tls_radio_is_dollman);
    LONG seen = 0;
    uint32_t before_count = 0;
    uint32_t after_count;
    uintptr_t pending_data;
    uintptr_t starttalk = 0;
    uintptr_t starttalk_vtbl = 0;
    uint32_t starttalk_flags = 0;
    if (!was_dollman) {
        if (g_real_queue_starttalk_function != NULL) {
            g_real_queue_starttalk_function(impl, payload);
        }
        return;
    }
    seen = InterlockedIncrement(&g_queue_starttalk_seen);
    before_count = safe_read_u32(impl + 0x18);
    if (was_dollman) {
        InterlockedIncrement(&g_queue_starttalk_tls_seen);
    }
    if (g_real_queue_starttalk_function == NULL) {
        if (seen <= 200) {
            log_verbose(
                "[queue-starttalk] seen=%ld real=NULL was_dollman=%d impl=0x%llx payload=0x%llx before_count=%u",
                (long)seen,
                was_dollman ? 1 : 0,
                (unsigned long long)impl,
                (unsigned long long)payload,
                (unsigned)before_count);
        }
        return;
    }
    g_real_queue_starttalk_function(impl, payload);
    after_count = safe_read_u32(impl + 0x18);
    pending_data = safe_read_ptr(impl + 0x20);
    if (pending_data != 0 && after_count != 0) {
        starttalk = safe_read_ptr(pending_data + (uintptr_t)(after_count - 1) * 8u);
        if (starttalk != 0) {
            starttalk_vtbl = safe_read_ptr(starttalk);
            starttalk_flags = safe_read_u32(starttalk + 0x68);
        }
    }
    if (seen <= 200) {
        log_verbose(
            "[queue-starttalk] seen=%ld was_dollman=%d impl=0x%llx payload=0x%llx before=%u after=%u pending=0x%llx starttalk=0x%llx vtbl=0x%llx flags=0x%x",
            (long)seen,
            was_dollman ? 1 : 0,
            (unsigned long long)impl,
            (unsigned long long)payload,
            (unsigned)before_count,
            (unsigned)after_count,
            (unsigned long long)pending_data,
            (unsigned long long)starttalk,
            (unsigned long long)starttalk_vtbl,
            (unsigned)starttalk_flags);
    }
    if (!was_dollman || !g_cfg.enable_subtitle_mute) {
        return;
    }
    {
        if (pending_data == 0 || after_count == 0) {
            return;
        }
        if (starttalk == 0) {
            return;
        }
        if (starttalk_vtbl != g_targets.vtbl_starttalk_function) {
            log_verbose(
                "[queue-starttalk] freshly pushed object at 0x%llx is not StartTalkFunction (vtbl=0x%llx)",
                (unsigned long long)starttalk,
                (unsigned long long)starttalk_vtbl);
            return;
        }
        *(uint8_t *)(starttalk + 0x68) |= 0x01u;
        remember_dollman_starttalk(starttalk);
        InterlockedIncrement(&g_starttalk_marked_dollman);
        log_verbose(
            "[queue-starttalk] marked dollman starttalk=0x%llx",
            (unsigned long long)starttalk);
    }
}

static uintptr_t __fastcall hook_starttalk_update_advance(uintptr_t self, double delta_seconds)
{
    BOOL was_dollman = FALSE;
    uintptr_t result = 0;
    if (runtime_enabled() && self != 0) {
        was_dollman = is_dollman_starttalk(self);
    }
    if (was_dollman) {
        tls_set_bool(g_tls_current_starttalk_is_dollman, TRUE);
        InterlockedIncrement(&g_subtitle_engine_skipped);
    }
    if (g_real_starttalk_update_advance != NULL) {
        result = g_real_starttalk_update_advance(self, delta_seconds);
    }
    if (was_dollman) {
        tls_set_bool(g_tls_current_starttalk_is_dollman, FALSE);
    }
    return result;
}

static uintptr_t *__fastcall hook_talksound_create_wrapper(
    uintptr_t *out, uintptr_t sound_instance)
{
    if (runtime_enabled() &&
        g_cfg.enable_voice_mute &&
        tls_get_bool(g_tls_current_starttalk_is_dollman)) {
        if (out != NULL) {
            *out = 0;
        }
        InterlockedIncrement(&g_voice_wrapper_substituted);
        log_verbose(
            "[talksound-wrap] muted dollman voice; out=%p sound=0x%llx",
            (void *)out, (unsigned long long)sound_instance);
        return out;
    }
    if (g_real_talksound_create_wrapper == NULL) {
        if (out != NULL) {
            *out = 0;
        }
        return out;
    }
    return g_real_talksound_create_wrapper(out, sound_instance);
}

static uintptr_t __fastcall hook_dollman_voice_schedule(uintptr_t self, int controller_index)
{
    LONG seen = 0;
    if (runtime_enabled()) {
        seen = InterlockedIncrement(&g_dollman_voice_schedule_seen);
        if (seen <= 64 || g_cfg.verbose_log) {
            uintptr_t self_vtbl = safe_read_ptr(self);
            uintptr_t voice_controller = safe_read_ptr(self + 0x10);
            uintptr_t playback_object = safe_read_ptr(self + 0x18);
            uintptr_t notification_queue = safe_read_ptr(self + 0x20);
            uintptr_t voice_vtbl = safe_read_ptr(voice_controller);
            uintptr_t playback_vtbl = safe_read_ptr(playback_object);
            uintptr_t queue_vtbl = safe_read_ptr(notification_queue);
            uintptr_t voice_list = safe_read_ptr(voice_controller + 0x70);
            uintptr_t voice_list0 = safe_read_ptr(voice_list);
            uintptr_t voice_list1 = safe_read_ptr(voice_list + 8);
            uintptr_t voice_list2 = safe_read_ptr(voice_list + 16);
            uint32_t voice_count = safe_read_u32(voice_controller + 0x60);
            uint32_t voice_flags = safe_read_u32(voice_controller + 0x64);
            uint32_t voice_list_count = safe_read_u32(voice_controller + 0x68);
            uint32_t group_count = safe_read_u32(self + 0x40);
            uint32_t group_flags = safe_read_u32(self + 0x44);
            log_line(
                "[dollman-voice-schedule] seen=%ld self=0x%llx vtbl=0x%llx controller=%d voice=0x%llx playback=0x%llx queue=0x%llx group_count=%u group_flags=0x%x",
                (long)seen,
                (unsigned long long)self,
                (unsigned long long)self_vtbl,
                controller_index,
                (unsigned long long)voice_controller,
                (unsigned long long)playback_object,
                (unsigned long long)notification_queue,
                (unsigned)group_count,
                (unsigned)group_flags);
            log_line(
                "[dollman-voice-probe] seen=%ld voice_vtbl=0x%llx playback_vtbl=0x%llx queue_vtbl=0x%llx voice_count=%u voice_flags=0x%x voice_list_count=%u voice_list=0x%llx list0=0x%llx list1=0x%llx list2=0x%llx",
                (long)seen,
                (unsigned long long)voice_vtbl,
                (unsigned long long)playback_vtbl,
                (unsigned long long)queue_vtbl,
                (unsigned)voice_count,
                (unsigned)voice_flags,
                (unsigned)voice_list_count,
                (unsigned long long)voice_list,
                (unsigned long long)voice_list0,
                (unsigned long long)voice_list1,
                (unsigned long long)voice_list2);
        }
    }
    if (g_real_dollman_voice_schedule == NULL) {
        return 0;
    }
    return g_real_dollman_voice_schedule(self, controller_index);
}

static void __fastcall hook_dollman_voice_closure(uintptr_t closure_payload)
{
    BOOL active = runtime_enabled();
    if (active) {
        LONG seen = InterlockedIncrement(&g_dollman_voice_closure_seen);
        uintptr_t self = safe_read_ptr(closure_payload);
        uint32_t controller_index = safe_read_u32(closure_payload + 8);
        tls_set_bool(g_tls_radio_is_dollman, TRUE);
        if (seen <= 64 || g_cfg.verbose_log) {
            log_line(
                "[dollman-voice-closure] seen=%ld payload=0x%llx self=0x%llx controller=%u",
                (long)seen,
                (unsigned long long)closure_payload,
                (unsigned long long)self,
                (unsigned)controller_index);
        }
    }
    if (g_real_dollman_voice_closure != NULL) {
        g_real_dollman_voice_closure(closure_payload);
    }
    if (active) {
        tls_set_bool(g_tls_radio_is_dollman, FALSE);
    }
}

static uint8_t __fastcall hook_voice_shared_helper(
    uintptr_t helper,
    uintptr_t voice_controller,
    uintptr_t notification_queue,
    int output_type,
    unsigned int *event_id)
{
    if (runtime_enabled() &&
        g_cfg.enable_voice_mute &&
        tls_get_bool(g_tls_radio_is_dollman)) {
        unsigned int id = event_id != NULL ? *event_id : 0;
        LONG consumed = InterlockedIncrement(&g_voice_helper_consumed);
        if (consumed <= 64 || g_cfg.verbose_log) {
            uintptr_t vc08 = safe_read_ptr(voice_controller + 0x08);
            uintptr_t vc10 = safe_read_ptr(voice_controller + 0x10);
            uintptr_t vc18 = safe_read_ptr(voice_controller + 0x18);
            uintptr_t vc20 = safe_read_ptr(voice_controller + 0x20);
            uintptr_t vc28 = safe_read_ptr(voice_controller + 0x28);
            uintptr_t vc30 = safe_read_ptr(voice_controller + 0x30);
            uintptr_t vc38 = safe_read_ptr(voice_controller + 0x38);
            uintptr_t vc40 = safe_read_ptr(voice_controller + 0x40);
            uintptr_t vc48 = safe_read_ptr(voice_controller + 0x48);
            uintptr_t vc50 = safe_read_ptr(voice_controller + 0x50);
            uintptr_t vc58 = safe_read_ptr(voice_controller + 0x58);
            uintptr_t vc80 = safe_read_ptr(voice_controller + 0x80);
            uintptr_t vc88 = safe_read_ptr(voice_controller + 0x88);
            uint32_t vc90 = safe_read_u32(voice_controller + 0x90);
            uint32_t vc94 = safe_read_u32(voice_controller + 0x94);
            uint32_t vc98 = safe_read_u32(voice_controller + 0x98);
            uintptr_t helper38 = safe_read_ptr(helper + 0x38);
            uintptr_t helper48 = safe_read_ptr(helper + 0x48);
            uint32_t helper40 = safe_read_u32(helper + 0x40);
            uint32_t helper60 = safe_read_u32(helper + 0x60);
            uint32_t helper64 = safe_read_u32(helper + 0x64);
            log_line(
                "[voice-helper] consumed=%ld dollman helper=0x%llx controller=0x%llx queue=0x%llx output=%d event=%u",
                (long)consumed,
                (unsigned long long)helper,
                (unsigned long long)voice_controller,
                (unsigned long long)notification_queue,
                output_type,
                id);
            log_line(
                "[voice-helper-probe] consumed=%ld vc08=0x%llx vc10=0x%llx vc18=0x%llx vc20=0x%llx vc28=0x%llx vc30=0x%llx vc38=0x%llx vc40=0x%llx vc48=0x%llx vc50=0x%llx vc58=0x%llx",
                (long)consumed,
                (unsigned long long)vc08,
                (unsigned long long)vc10,
                (unsigned long long)vc18,
                (unsigned long long)vc20,
                (unsigned long long)vc28,
                (unsigned long long)vc30,
                (unsigned long long)vc38,
                (unsigned long long)vc40,
                (unsigned long long)vc48,
                (unsigned long long)vc50,
                (unsigned long long)vc58);
            log_line(
                "[voice-helper-state] consumed=%ld vc80=0x%llx vc88=0x%llx vc90=0x%x vc94=0x%x vc98=0x%x helper38=0x%llx helper40=%u helper48=0x%llx helper60=%u helper64=%u",
                (long)consumed,
                (unsigned long long)vc80,
                (unsigned long long)vc88,
                (unsigned)vc90,
                (unsigned)vc94,
                (unsigned)vc98,
                (unsigned long long)helper38,
                (unsigned)helper40,
                (unsigned long long)helper48,
                (unsigned)helper60,
                (unsigned)helper64);
        }
        return 1;
    }
    if (g_real_voice_shared_helper == NULL) {
        return 0;
    }
    return g_real_voice_shared_helper(
        helper, voice_controller, notification_queue, output_type, event_id);
}

/* ------------------------------------------------------------------------- */
/* Hook installation                                                          */
/* ------------------------------------------------------------------------- */

typedef struct HookSpec {
    const char *label;
    uintptr_t target;
    void *detour;
    void **original;
    BOOL required;
    BOOL enabled;
} HookSpec;

static int install_hooks(void)
{
    HookSpec specs[] = {
        { "RadioVoiceDispatcher (DollmanInstance.vtable[2])",
          g_targets.fn_radio_voice_dispatcher,
          (void *)hook_radio_voice_dispatcher,
          (void **)&g_real_radio_voice_dispatcher,
          TRUE,
          g_cfg.hook_radio_dispatcher },
        { "EchobackEnqueue (v1.7 RVA fallback)",
          g_targets.fn_echoback_enqueue,
          (void *)hook_echoback_enqueue,
          (void **)&g_real_echoback_enqueue,
          FALSE,
          g_cfg.hook_echoback },
        { "EchobackFunction.vtable[2] Execute",
          g_targets.fn_echoback_execute,
          (void *)hook_echoback_execute,
          (void **)&g_real_echoback_execute,
          FALSE,
          g_cfg.hook_echoback },
        { "QueueStartTalkFunction (v1.7 RVA fallback)",
          g_targets.fn_queue_starttalk_function,
          (void *)hook_queue_starttalk_function,
          (void **)&g_real_queue_starttalk_function,
          FALSE,
          g_cfg.hook_queue_starttalk },
        { "StartTalkFunction.vtable[15] UpdateAdvance",
          g_targets.fn_starttalk_update_advance,
          (void *)hook_starttalk_update_advance,
          (void **)&g_real_starttalk_update_advance,
          FALSE,
          g_cfg.hook_starttalk_update },
        { "TalkSound_CreateSoundInstanceWrapper (v1.7 RVA fallback)",
          g_targets.fn_talksound_create_wrapper,
          (void *)hook_talksound_create_wrapper,
          (void **)&g_real_talksound_create_wrapper,
          FALSE,
          g_cfg.hook_talksound_wrapper },
        { "DollmanVoiceDelaySchedule (DollmanInstance.vtable[8])",
          g_targets.fn_dollman_voice_schedule,
          (void *)hook_dollman_voice_schedule,
          (void **)&g_real_dollman_voice_schedule,
          FALSE,
          g_cfg.hook_dollman_voice_schedule },
        { "DollmanVoiceDelayClosure (v1.7 RVA fallback)",
          g_targets.fn_dollman_voice_closure,
          (void *)hook_dollman_voice_closure,
          (void **)&g_real_dollman_voice_closure,
          FALSE,
          g_cfg.hook_dollman_voice_closure },
        { "VoiceSharedHelper (v1.7 RVA fallback)",
          g_targets.fn_voice_shared_helper,
          (void *)hook_voice_shared_helper,
          (void **)&g_real_voice_shared_helper,
          FALSE,
          g_cfg.hook_voice_shared_helper },
    };
    int installed = 0;
    size_t i;

    for (i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        const HookSpec *s = &specs[i];
        MH_STATUS status;
        if (!s->enabled) {
            log_line("Hook skip: %s (disabled via config)", s->label);
            continue;
        }
        if (s->target == 0) {
            log_line("Hook skip: %s (target=0)", s->label);
            if (s->required) {
                return -1;
            }
            continue;
        }
        status = MH_CreateHook((LPVOID)s->target, s->detour, s->original);
        if (status != MH_OK) {
            log_line("MH_CreateHook(%s) failed: %d", s->label, (int)status);
            if (s->required) {
                return -1;
            }
            continue;
        }
        status = MH_EnableHook((LPVOID)s->target);
        if (status != MH_OK) {
            log_line("MH_EnableHook(%s) failed: %d", s->label, (int)status);
            if (s->required) {
                return -1;
            }
            continue;
        }
        log_line("Hooked %s at 0x%llx", s->label, (unsigned long long)s->target);
        installed++;
    }
    return installed;
}

static DWORD WINAPI hotkey_thread_proc(LPVOID param)
{
    SHORT prev_state = 0;
    int vk;
    (void)param;

    vk = g_cfg.toggle_hotkey_vk;
    if (vk <= 0) {
        return 0;
    }
    log_line("Hotkey active: VK=0x%x toggles DollmanMute runtime", (unsigned)vk);
    while (InterlockedCompareExchange(&g_core_shutting_down, 0, 0) == 0) {
        SHORT state = GetAsyncKeyState(vk);
        BOOL pressed = (state & 0x8000) != 0;
        BOOL was_pressed = (prev_state & 0x8000) != 0;
        if (pressed && !was_pressed) {
            LONG old_value;
            LONG new_value;
            do {
                old_value = InterlockedCompareExchange(&g_runtime_enabled, 0, 0);
                new_value = old_value ? 0 : 1;
            } while (InterlockedCompareExchange(&g_runtime_enabled, new_value, old_value) != old_value);
            if (!new_value) {
                clear_state_tracking();
                set_hooks_runtime_enabled(FALSE);
            } else {
                set_hooks_runtime_enabled(TRUE);
            }
            log_line(
                "Runtime toggle: DollmanMute %s via VK=0x%x",
                new_value ? "enabled" : "disabled",
                (unsigned)vk);
        }
        prev_state = state;
        Sleep(50);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Init / shutdown                                                            */
/* ------------------------------------------------------------------------- */

__declspec(dllexport) int core_init(const ProxyContext *ctx)
{
    int installed;

    ZeroMemory(&g_proxy_ctx, sizeof(g_proxy_ctx));
    InterlockedExchange(&g_core_shutting_down, 0);
    if (ctx != NULL) {
        g_proxy_ctx = *ctx;
    }

    InitializeCriticalSection(&g_log_lock);
    g_log_lock_inited = TRUE;
    InitializeCriticalSection(&g_state_lock);
    g_state_lock_inited = TRUE;
    clear_state_tracking();
    init_paths();
    load_config();
    InterlockedExchange(&g_runtime_enabled, g_cfg.enabled ? 1 : 0);

    g_tls_radio_is_dollman = TlsAlloc();
    g_tls_current_starttalk_is_dollman = TlsAlloc();

    log_line("DollmanMute build: %s", k_build_tag);
    log_line(
        "Config: enabled=%d verbose=%d voice=%d subtitle=%d toggle_vk=0x%x hooks={radio=%d echoback=%d queue=%d update=%d wrap=%d schedule=%d closure=%d helper=%d}",
        g_cfg.enabled ? 1 : 0,
        g_cfg.verbose_log ? 1 : 0,
        g_cfg.enable_voice_mute ? 1 : 0,
        g_cfg.enable_subtitle_mute ? 1 : 0,
        (unsigned)g_cfg.toggle_hotkey_vk,
        g_cfg.hook_radio_dispatcher ? 1 : 0,
        g_cfg.hook_echoback ? 1 : 0,
        g_cfg.hook_queue_starttalk ? 1 : 0,
        g_cfg.hook_starttalk_update ? 1 : 0,
        g_cfg.hook_talksound_wrapper ? 1 : 0,
        g_cfg.hook_dollman_voice_schedule ? 1 : 0,
        g_cfg.hook_dollman_voice_closure ? 1 : 0,
        g_cfg.hook_voice_shared_helper ? 1 : 0);

    if (!g_cfg.enabled) {
        log_line("Disabled via config; skipping hook install");
        return 0;
    }
    if (!pe_introspect()) {
        return 0;
    }
    if (!resolve_all()) {
        log_line("Resolution failed; skipping hook install");
        return 0;
    }
    if (MH_Initialize() != MH_OK) {
        log_line("MH_Initialize failed");
        return 0;
    }
    installed = install_hooks();
    if (installed < 0) {
        log_line("Required hook missing; rolling back");
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return 0;
    }
    InterlockedExchange(&g_hooks_runtime_enabled, installed > 0 ? 1 : 0);
    log_line("DollmanMute init complete: hooks=%d", installed);
    if (g_cfg.toggle_hotkey_vk > 0) {
        g_hotkey_thread = CreateThread(NULL, 0, hotkey_thread_proc, NULL, 0, NULL);
        if (g_hotkey_thread == NULL) {
            log_line("Hotkey thread failed to start");
        }
    }
    return 0;
}

__declspec(dllexport) void core_shutdown(void)
{
    InterlockedExchange(&g_core_shutting_down, 1);
    log_line("core_shutdown begin");
    if (g_hotkey_thread != NULL) {
        DWORD wait_result = WaitForSingleObject(g_hotkey_thread, 500);
        if (wait_result != WAIT_OBJECT_0) {
            log_line("core_shutdown: hotkey thread did not exit within 500ms");
        }
        CloseHandle(g_hotkey_thread);
        g_hotkey_thread = NULL;
    }
    InterlockedExchange(&g_hooks_runtime_enabled, 0);
    log_line(
        "Counters: radio_dollman=%ld echoback_marked=%ld echoback_exec=%ld queue_seen=%ld queue_tls=%ld starttalk_marked=%ld voice_substituted=%ld schedule_seen=%ld closure_seen=%ld helper_consumed=%ld subtitle_skipped=%ld",
        (long)g_radio_dispatcher_dollman_hits,
        (long)g_echoback_marked_dollman,
        (long)g_echoback_executed_dollman,
        (long)g_queue_starttalk_seen,
        (long)g_queue_starttalk_tls_seen,
        (long)g_starttalk_marked_dollman,
        (long)g_voice_wrapper_substituted,
        (long)g_dollman_voice_schedule_seen,
        (long)g_dollman_voice_closure_seen,
        (long)g_voice_helper_consumed,
        (long)g_subtitle_engine_skipped);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (g_tls_radio_is_dollman != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_radio_is_dollman);
        g_tls_radio_is_dollman = TLS_OUT_OF_INDEXES;
    }
    if (g_tls_current_starttalk_is_dollman != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_current_starttalk_is_dollman);
        g_tls_current_starttalk_is_dollman = TLS_OUT_OF_INDEXES;
    }
    log_line("core_shutdown complete");
    if (g_log_lock_inited) {
        DeleteCriticalSection(&g_log_lock);
        g_log_lock_inited = FALSE;
    }
    if (g_state_lock_inited) {
        DeleteCriticalSection(&g_state_lock);
        g_state_lock_inited = FALSE;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

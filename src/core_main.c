#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "MinHook.h"
#include "core_api.h"

typedef uint32_t AkUniqueID;
typedef uint32_t AkPlayingID;
typedef uint64_t AkGameObjectID;

typedef struct Config {
    BOOL enabled;
    BOOL verbose_log;
    BOOL enable_dollman_radio_mute;
    BOOL enable_throw_recall_subtitle_mute;
    BOOL enable_subtitle_producer_probe;
    BOOL enable_builder_probe;
    BOOL enable_selector_probe;
    uint32_t scanner_mode;
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
typedef uintptr_t(__fastcall *DollmanRadioPlayVoiceByControllerDelayFn)(
    uintptr_t instance,
    int controller_index);
typedef uintptr_t(__fastcall *DollmanVoiceDispatcherFn)(
    uintptr_t lock_obj,
    uintptr_t radio_instance,
    uintptr_t param32,
    int mode,
    int *sentence_key);
typedef uintptr_t(__fastcall *ShowSubtitleFn)(uintptr_t view, const uint64_t *payload);
typedef uintptr_t(__fastcall *SubtitleProducerFn)(uintptr_t this_obj);

static HMODULE g_self_module = NULL;
static CRITICAL_SECTION g_log_lock;
static BOOL g_log_lock_inited = FALSE;
static Config g_cfg;
static char g_ini_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static ProxyContext g_proxy_ctx;
static volatile LONG g_core_shutting_down = 0;
static HANDLE g_hotkey_thread_handle = NULL;

static PostEventIdFn g_real_post_event_id = NULL;
static DollmanRadioPlayVoiceByControllerDelayFn g_real_dollman_radio_play_voice_by_controller_delay = NULL;
static DollmanVoiceDispatcherFn g_real_dollman_voice_dispatcher = NULL;
static ShowSubtitleFn g_real_show_subtitle = NULL;
static SubtitleProducerFn g_real_subtitle_producer = NULL;

static const char *k_build_tag = "public-release-v3.19-producer-prepost-snapshot";

#define PRODUCER_IDENTITY_CACHE_MAX 4096
static uintptr_t g_image_base = 0;
static uintptr_t g_image_size = 0;
static const uintptr_t k_rva_localized_text_resource_vtbl = 0x03448E48u;

static const char *classify_builder_c_msg(
    uintptr_t msg_vtbl_rva,
    uint64_t msg1,
    uint64_t msg2,
    uint64_t msg3)
{
    if (msg_vtbl_rva == 0x3117438) return "pass_vtbl_B";
    if (msg_vtbl_rva != 0x3131e38) return "pass_vtbl_other";
    if (g_image_base != 0 && (msg1 >> 32) == ((uint64_t)g_image_base >> 32)) {
        return "pass_code_ra";
    }
    if (msg1 == 0 && msg3 == 0) return "pass_empty";
    if (msg1 == 0xFFFFFF00ull) return "block_hazard";
    if (msg2 == 0x3f99999aull) return "block_hazard_variant";
    if (msg3 == 0xADull) return "block_pool_ad";
    if ((msg2 & 0xFFFFFFFFull) == 0x3fc00000ull) return "block_pool_1p5";
    if ((msg2 >> 32) == 2ull) return "block_chatter";
    if ((msg2 & 0xFFFFFFFFull) == 0x40933333ull) return "block_chatter_4p6";
    return "unknown";
}
static CRITICAL_SECTION g_identity_lock;
static BOOL g_identity_lock_inited = FALSE;
static uintptr_t g_identity_cache[PRODUCER_IDENTITY_CACHE_MAX];
static int g_identity_cache_count = 0;
static BOOL g_identity_cache_full_warned = FALSE;
static CRITICAL_SECTION g_hotkey_lock;
static BOOL g_hotkey_lock_inited = FALSE;
static BOOL g_hotkey_throw_recall_mute = FALSE;
static BOOL g_hotkey_dialogue_mute = FALSE;
static BOOL g_hotkey_j_prev = FALSE;
static BOOL g_hotkey_k_prev = FALSE;
static BOOL g_hotkey_n_prev = FALSE;
static BOOL g_hotkey_m_prev = FALSE;
static BOOL g_hotkey_f12_prev = FALSE;
static DWORD g_tls_muted_subtitle_family = TLS_OUT_OF_INDEXES;

static const char *k_export_post_event_id =
    "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";

static const uintptr_t k_rva_dollman_radio_play_voice_by_controller_delay = 0x00C73DF0u;
static const uintptr_t k_rva_dollman_voice_dispatcher = 0x00DAC1B0u;
static const uintptr_t k_rva_show_subtitle = 0x00780710u;
static const uintptr_t k_rva_subtitle_producer = 0x003873E0u;
static const uintptr_t k_rva_selector_dispatch = 0x00DAFAD0u;
static const uintptr_t k_rva_talk_dispatcher = 0x003857E0u;

/* v3.17 gameplay Dollman mute: observed (speaker tag, ShowSubtitle caller RVA)
 * pair for the chatter path. Cutscene Dollman uses the same speaker tag but
 * comes through caller RVA 0x202f16f, and anonymous Dollman (pre-naming) uses
 * speaker tag 0x12b70 — both must NOT match this pair. */
static const uint32_t k_dollman_gameplay_speaker_tag = 0x12b6fu;
static const uintptr_t k_dollman_gameplay_caller_rva = 0x38598bu;

static const AkUniqueID k_scanner_event_id_1 = 4235852663u;
static const AkUniqueID k_scanner_event_id_2 = 4094913469u;
static const AkUniqueID k_scanner_event_id_3 = 2611919341u;

enum {
    SCANNER_MODE_OFF = 0u,
    SCANNER_MODE_REDUCED = 1u,
    SCANNER_MODE_MUTE_ALL = 2u
};

enum {
    SUBTITLE_FAMILY_NONE = 0u,
    SUBTITLE_FAMILY_THROW_RECALL = 1u,
    SUBTITLE_FAMILY_DIALOGUE = 2u
};

static const uint32_t k_speaker_tag_throw_recall = 0x01F4u;
static const uint32_t k_speaker_tag_dialogue_a = 0x222Cu;
static const uint32_t k_speaker_tag_dialogue_b = 0x4377u;

/* --- Builder probe (v3.14): log-only hooks on gameplay subtitle builders --- */
enum {
    BUILDER_ID_NONE = 0u,
    BUILDER_ID_A    = 1u, /* sub_140350050 — single candidate pool */
    BUILDER_ID_B    = 2u, /* sub_140350460 — primary entry + variant array */
    BUILDER_ID_C    = 3u, /* sub_140350960 — pre-built entry array */
    BUILDER_ID_U1   = 4u, /* sub_140B5BC70 — unclassified 0x388950 caller */
    BUILDER_ID_U2   = 5u, /* sub_140B5CC30 — unclassified 0x388950 caller */
    BUILDER_ID_COUNT = 6u
};

static const char *k_builder_names[BUILDER_ID_COUNT] = {
    "none", "A", "B", "C", "U1", "U2"
};

static const uintptr_t k_rva_builder_a  = 0x00350050u;
static const uintptr_t k_rva_builder_b  = 0x00350460u;
static const uintptr_t k_rva_builder_c  = 0x00350960u;
static const uintptr_t k_rva_builder_u1 = 0x00B5BC70u;
static const uintptr_t k_rva_builder_u2 = 0x00B5CC30u;

typedef uintptr_t (__fastcall *BuilderFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef uintptr_t (__fastcall *SelectorDispatchFn)(uintptr_t, uintptr_t);
typedef uintptr_t (__fastcall *TalkDispatcherFn)(uintptr_t *a1, uintptr_t *i);

static void *g_real_builder_a  = NULL;
static void *g_real_builder_b  = NULL;
static void *g_real_builder_c  = NULL;
static void *g_real_builder_u1 = NULL;
static void *g_real_builder_u2 = NULL;
static SelectorDispatchFn g_real_selector_dispatch = NULL;
static TalkDispatcherFn g_real_talk_dispatcher = NULL;

static volatile LONG g_builder_hit_counts[BUILDER_ID_COUNT] = {0};
static DWORD g_tls_last_builder = TLS_OUT_OF_INDEXES;

static BOOL g_hotkey_f8_prev = FALSE;
static BOOL g_hotkey_f9_prev = FALSE;
static volatile LONG g_session_counter = 0;

static const AkUniqueID k_blocked_event_ids[] = {
    2995625663u, /* equip */
    2820786646u, /* throw */
    2978848044u, /* recall */
    1966841225u, /* random chatter */
    302733266u,  /* task-failure */
    448888368u   /* fall chatter */
};

static uintptr_t safe_deref_qword(uintptr_t addr);
static BOOL read_localized_text_resource(
    uintptr_t ptr,
    uint32_t *tag_out,
    char *text_buffer,
    size_t text_buffer_size);
static void log_builder_c_post_probe(uintptr_t rcx, uintptr_t result);
static void log_start_talk_function_snapshot(const char *phase, uintptr_t this_obj);

static const char *k_default_ini =
    "; DollmanMute runtime config.\n"
    "; VerboseLog=1 enables per-event mute logging for debugging.\n"
    "; EnableDollmanRadioMute=1 enables the old audio mute path.\n"
    ";   Keep 0 while testing the new subtitle-family hotkeys.\n"
    ";   (hat/glasses reactions etc) together with their subtitles.\n"
    "; EnableThrowRecallSubtitleMute=1 sets startup default for throw/recall mute.\n"
    "; EnableBuilderProbe=1 installs log-only hooks on gameplay subtitle builders\n"
    ";   A=sub_140350050, B=sub_140350460, C=sub_140350960,\n"
    ";   U1=sub_140B5BC70, U2=sub_140B5CC30, and emits [show]/[builder] log lines.\n"
    ";   Used for mapping dollman behaviours to builder paths.\n"
    "; Runtime hotkeys:\n"
    ";   J = throw/recall mute ON\n"
    ";   K = throw/recall mute OFF\n"
    ";   N = dollman dialogue mute ON\n"
    ";   M = dollman dialogue mute OFF\n"
    ";   F12 = clear all runtime subtitle mutes\n"
    ";   F8  = mark session boundary in log (builder probe aid)\n"
    ";   F9  = truncate DollmanMute.log (fresh capture)\n"
    ";   F10 = reload core (proxy feature, not handled here)\n"
    "; EnableSubtitleProducerProbe is kept only for offline research builds.\n"
    "; ScannerMode=0 keeps scanner audio unchanged.\n"
    "; ScannerMode=1 reduces scanner intensity.\n"
    "; ScannerMode=2 fully mutes scanner audio.\n"
    "\n"
    "[General]\n"
    "Enabled=1\n"
    "VerboseLog=0\n"
    "EnableDollmanRadioMute=0\n"
    "EnableThrowRecallSubtitleMute=1\n"
    "EnableSubtitleProducerProbe=1\n"
    "EnableBuilderProbe=1\n"
    "ScannerMode=0\n";

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
    g_cfg.enable_dollman_radio_mute = FALSE;
    g_cfg.enable_throw_recall_subtitle_mute = TRUE;
    g_cfg.enable_subtitle_producer_probe = FALSE;
    g_cfg.enable_builder_probe = FALSE;
    g_cfg.enable_selector_probe = TRUE;
    g_cfg.scanner_mode = SCANNER_MODE_OFF;

    ensure_default_ini();

    g_cfg.enabled = GetPrivateProfileIntA("General", "Enabled", g_cfg.enabled, g_ini_path) != 0;
    g_cfg.verbose_log = GetPrivateProfileIntA("General", "VerboseLog", g_cfg.verbose_log, g_ini_path) != 0;
    g_cfg.enable_dollman_radio_mute = GetPrivateProfileIntA(
        "General",
        "EnableDollmanRadioMute",
        g_cfg.enable_dollman_radio_mute,
        g_ini_path) != 0;
    g_cfg.enable_throw_recall_subtitle_mute = GetPrivateProfileIntA(
        "General",
        "EnableThrowRecallSubtitleMute",
        g_cfg.enable_throw_recall_subtitle_mute,
        g_ini_path) != 0;
    g_cfg.enable_subtitle_producer_probe = GetPrivateProfileIntA(
        "General",
        "EnableSubtitleProducerProbe",
        g_cfg.enable_subtitle_producer_probe,
        g_ini_path) != 0;
    g_cfg.enable_builder_probe = GetPrivateProfileIntA(
        "General",
        "EnableBuilderProbe",
        g_cfg.enable_builder_probe,
        g_ini_path) != 0;
    g_cfg.enable_selector_probe = GetPrivateProfileIntA(
        "General",
        "EnableSelectorProbe",
        g_cfg.enable_selector_probe,
        g_ini_path) != 0;
    {
        int scanner_mode_value = GetPrivateProfileIntA(
            "General",
            "ScannerMode",
            (int)g_cfg.scanner_mode,
            g_ini_path);
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

    if (g_log_lock_inited) {
        EnterCriticalSection(&g_log_lock);
    }
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

static void clear_log_file(void)
{
    HANDLE file;

    if (g_log_path[0] == '\0') {
        return;
    }

    if (g_log_lock_inited) {
        EnterCriticalSection(&g_log_lock);
    }
    file = CreateFileA(
        g_log_path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    if (g_log_lock_inited) {
        LeaveCriticalSection(&g_log_lock);
    }
}

static const char *subtitle_family_name(uint32_t family)
{
    switch (family) {
    case SUBTITLE_FAMILY_THROW_RECALL:
        return "throwRecall";
    case SUBTITLE_FAMILY_DIALOGUE:
        return "dialogue";
    default:
        return "none";
    }
}

static BOOL is_vk_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void seed_hotkey_state_from_config(void)
{
    EnterCriticalSection(&g_hotkey_lock);
    g_hotkey_throw_recall_mute = g_cfg.enable_throw_recall_subtitle_mute;
    g_hotkey_dialogue_mute = FALSE;
    LeaveCriticalSection(&g_hotkey_lock);
}

static void get_hotkey_mute_state(BOOL *throw_recall_mute, BOOL *dialogue_mute)
{
    EnterCriticalSection(&g_hotkey_lock);
    if (throw_recall_mute != NULL) {
        *throw_recall_mute = g_hotkey_throw_recall_mute;
    }
    if (dialogue_mute != NULL) {
        *dialogue_mute = g_hotkey_dialogue_mute;
    }
    LeaveCriticalSection(&g_hotkey_lock);
}

static BOOL should_mute_subtitle_family(uint32_t family)
{
    BOOL throw_recall_mute = FALSE;
    BOOL dialogue_mute = FALSE;

    get_hotkey_mute_state(&throw_recall_mute, &dialogue_mute);

    if (family == SUBTITLE_FAMILY_THROW_RECALL) {
        return throw_recall_mute;
    }
    if (family == SUBTITLE_FAMILY_DIALOGUE) {
        return dialogue_mute;
    }
    return FALSE;
}

static uintptr_t tls_get_muted_subtitle_family(void)
{
    if (g_tls_muted_subtitle_family == TLS_OUT_OF_INDEXES) {
        return SUBTITLE_FAMILY_NONE;
    }
    return (uintptr_t)TlsGetValue(g_tls_muted_subtitle_family);
}

static void tls_set_muted_subtitle_family(uintptr_t family)
{
    if (g_tls_muted_subtitle_family == TLS_OUT_OF_INDEXES) {
        return;
    }
    TlsSetValue(g_tls_muted_subtitle_family, (LPVOID)family);
}

static uint32_t tls_get_last_builder(void)
{
    if (g_tls_last_builder == TLS_OUT_OF_INDEXES) {
        return BUILDER_ID_NONE;
    }
    return (uint32_t)(uintptr_t)TlsGetValue(g_tls_last_builder);
}

static void tls_set_last_builder(uint32_t id)
{
    if (g_tls_last_builder == TLS_OUT_OF_INDEXES) {
        return;
    }
    TlsSetValue(g_tls_last_builder, (LPVOID)(uintptr_t)id);
}

static uint32_t read_identity_hi32(uintptr_t ptr)
{
    uint64_t word1 = safe_deref_qword(ptr + sizeof(uint64_t));
    return (uint32_t)(word1 >> 32);
}

static uint32_t classify_subtitle_family(uintptr_t pre_p200_field72)
{
    uint32_t speaker_tag = read_identity_hi32(pre_p200_field72);

    if (speaker_tag == k_speaker_tag_throw_recall) {
        return SUBTITLE_FAMILY_THROW_RECALL;
    }
    if (speaker_tag == k_speaker_tag_dialogue_a || speaker_tag == k_speaker_tag_dialogue_b) {
        return SUBTITLE_FAMILY_DIALOGUE;
    }
    return SUBTITLE_FAMILY_NONE;
}

static void update_hotkey_mute_state(void)
{
    BOOL key_j_down = is_vk_down('J');
    BOOL key_k_down = is_vk_down('K');
    BOOL key_n_down = is_vk_down('N');
    BOOL key_m_down = is_vk_down('M');
    BOOL key_f12_down = is_vk_down(VK_F12);
    BOOL key_f8_down = is_vk_down(VK_F8);
    BOOL key_f9_down = is_vk_down(VK_F9);

    if (key_f8_down && !g_hotkey_f8_prev) {
        LONG counter = InterlockedIncrement(&g_session_counter);
        log_line("=== session boundary F8 count=%ld ===", (long)counter);
    }
    g_hotkey_f8_prev = key_f8_down;

    if (key_f9_down && !g_hotkey_f9_prev) {
        clear_log_file();
        log_line("=== log cleared by F9 ===");
    }
    g_hotkey_f9_prev = key_f9_down;

    EnterCriticalSection(&g_hotkey_lock);

    if (key_j_down && !g_hotkey_j_prev) {
        g_hotkey_throw_recall_mute = TRUE;
        log_line(
            "HotkeyMute throwRecall=%d dialogue=%d key='J'",
            g_hotkey_throw_recall_mute ? 1 : 0,
            g_hotkey_dialogue_mute ? 1 : 0);
    }
    if (key_k_down && !g_hotkey_k_prev) {
        g_hotkey_throw_recall_mute = FALSE;
        log_line(
            "HotkeyMute throwRecall=%d dialogue=%d key='K'",
            g_hotkey_throw_recall_mute ? 1 : 0,
            g_hotkey_dialogue_mute ? 1 : 0);
    }
    if (key_n_down && !g_hotkey_n_prev) {
        g_hotkey_dialogue_mute = TRUE;
        log_line(
            "HotkeyMute throwRecall=%d dialogue=%d key='N'",
            g_hotkey_throw_recall_mute ? 1 : 0,
            g_hotkey_dialogue_mute ? 1 : 0);
    }
    if (key_m_down && !g_hotkey_m_prev) {
        g_hotkey_dialogue_mute = FALSE;
        log_line(
            "HotkeyMute throwRecall=%d dialogue=%d key='M'",
            g_hotkey_throw_recall_mute ? 1 : 0,
            g_hotkey_dialogue_mute ? 1 : 0);
    }
    if (key_f12_down && !g_hotkey_f12_prev) {
        g_hotkey_throw_recall_mute = FALSE;
        g_hotkey_dialogue_mute = FALSE;
        log_line("HotkeyMute throwRecall=0 dialogue=0 key='F12'");
    }

    g_hotkey_j_prev = key_j_down;
    g_hotkey_k_prev = key_k_down;
    g_hotkey_n_prev = key_n_down;
    g_hotkey_m_prev = key_m_down;
    g_hotkey_f12_prev = key_f12_down;

    LeaveCriticalSection(&g_hotkey_lock);
}

static DWORD WINAPI hotkey_thread_proc(LPVOID parameter)
{
    (void)parameter;

    while (InterlockedCompareExchange(&g_core_shutting_down, 0, 0) == 0) {
        update_hotkey_mute_state();
        Sleep(10);
    }

    return 0;
}

static BOOL should_block_event_id(AkUniqueID event_id)
{
    size_t i;

    if (event_id == k_scanner_event_id_1) {
        return g_cfg.scanner_mode == SCANNER_MODE_MUTE_ALL;
    }
    if (event_id == k_scanner_event_id_2 || event_id == k_scanner_event_id_3) {
        return g_cfg.scanner_mode == SCANNER_MODE_MUTE_ALL ||
               g_cfg.scanner_mode == SCANNER_MODE_REDUCED;
    }

    if (!g_cfg.enable_dollman_radio_mute) {
        return FALSE;
    }

    for (i = 0; i < sizeof(k_blocked_event_ids) / sizeof(k_blocked_event_ids[0]); ++i) {
        if (k_blocked_event_ids[i] == event_id) {
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
            blocked ? "Blocked" : "Seen",
            (unsigned int)event_id,
            (unsigned long long)game_object_id,
            (unsigned int)external_source_count);
    }

    if (blocked) {
        (void)callback_mask;
        (void)callback;
        (void)cookie;
        (void)external_sources;
        (void)playing_id;
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

static uintptr_t __fastcall hook_dollman_radio_play_voice_by_controller_delay(
    uintptr_t instance,
    int controller_index)
{
    if (g_cfg.enabled && g_cfg.enable_dollman_radio_mute) {
        log_verbose(
            "Muted DollmanRadio.PlayVoiceByControllerDelay instance=%p controller=%d",
            (void *)instance,
            controller_index);
        return 0;
    }

    return g_real_dollman_radio_play_voice_by_controller_delay(instance, controller_index);
}

static uintptr_t __fastcall hook_dollman_voice_dispatcher(
    uintptr_t lock_obj,
    uintptr_t radio_instance,
    uintptr_t param32,
    int mode,
    int *sentence_key)
{
    if (g_cfg.enabled && g_cfg.enable_dollman_radio_mute) {
        log_verbose(
            "Muted DollmanVoiceDispatcher radio=%p mode=%d key=0x%x param=%p",
            (void *)radio_instance,
            mode,
            (sentence_key != NULL) ? (unsigned int)*sentence_key : 0u,
            (void *)param32);
        return 1;
    }

    return g_real_dollman_voice_dispatcher(lock_obj, radio_instance, param32, mode, sentence_key);
}

static uintptr_t __fastcall hook_talk_dispatcher(uintptr_t *a1, uintptr_t *i)
{
    /* v3.16 probe: sub_1403857E0 is DSTalkInternal::StartTalkFunction's event
     * broadcaster. a1 = StartTalkFunction this-ptr. Speaker/line object chain:
     *   a1[3]                      -> line header (v10 in IDA)
     *   *(a1[3] + 376)             -> line_data (result in IDA)
     *   *(*(a1[3] + 376) + 0)      -> line_data->vtbl (speaker class)
     * The vtbl RVA uniquely identifies dollman vs Sam/NPC/cutscene talk nodes.
     * Probe-only: logs the chain, always calls the real dispatcher. */
    if (g_cfg.enabled && g_cfg.enable_selector_probe &&
        a1 != NULL && !IsBadReadPtr(a1, 4 * sizeof(uintptr_t))) {
        uintptr_t a1_0 = a1[0];
        uintptr_t a1_1 = a1[1];
        uintptr_t a1_2 = a1[2];
        uintptr_t a1_3 = a1[3];
        uintptr_t line_data = 0;
        uintptr_t line_vtbl = 0;
        uintptr_t line_vtbl_rva = 0;
        uintptr_t tf_vtbl = safe_deref_qword(a1_0);
        uintptr_t tf_vtbl_rva = (g_image_base != 0 && tf_vtbl > g_image_base)
                                    ? (tf_vtbl - g_image_base) : 0;
        if (a1_3 != 0 && !IsBadReadPtr((const void *)a1_3, 384)) {
            line_data = *(const uintptr_t *)(a1_3 + 376);
            if (line_data != 0 && !IsBadReadPtr((const void *)line_data, sizeof(uintptr_t))) {
                line_vtbl = *(const uintptr_t *)line_data;
                if (g_image_base != 0 && line_vtbl > g_image_base) {
                    line_vtbl_rva = line_vtbl - g_image_base;
                }
            }
        }
        log_line(
            "[disp] a1=%p tf_vtbl_rva=0x%llx a1_1=0x%llx a1_2=0x%llx a1_3=0x%llx line=0x%llx line_vtbl_rva=0x%llx tid=%lu",
            (void *)a1,
            (unsigned long long)tf_vtbl_rva,
            (unsigned long long)a1_1,
            (unsigned long long)a1_2,
            (unsigned long long)a1_3,
            (unsigned long long)line_data,
            (unsigned long long)line_vtbl_rva,
            (unsigned long)GetCurrentThreadId());
    }
    return g_real_talk_dispatcher(a1, i);
}

static uintptr_t __fastcall hook_show_subtitle(uintptr_t view, const uint64_t *payload)
{
    uintptr_t caller_ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t muted_family = tls_get_muted_subtitle_family();
    uint32_t last_builder = tls_get_last_builder();
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                               ? (caller_ra - g_image_base)
                               : 0;

    if (g_cfg.enable_builder_probe || g_cfg.enable_selector_probe) {
        uint64_t words[8] = {0};
        uint64_t p6_data[8] = {0};
        uint64_t p7_data[8] = {0};
        BOOL readable = FALSE;
        uintptr_t vtbl_rva = 0;
        uintptr_t p6_vtbl_rva = 0;
        uintptr_t p7_vtbl_rva = 0;
        if (payload != NULL && !IsBadReadPtr(payload, sizeof(words))) {
            memcpy(words, payload, sizeof(words));
            readable = TRUE;
            if (g_image_base != 0 && (uintptr_t)words[0] > g_image_base) {
                vtbl_rva = (uintptr_t)words[0] - g_image_base;
            }
            if (words[6] != 0 &&
                !IsBadReadPtr((const void *)(uintptr_t)words[6], sizeof(p6_data))) {
                memcpy(p6_data, (const void *)(uintptr_t)words[6], sizeof(p6_data));
                if (g_image_base != 0 && (uintptr_t)p6_data[0] > g_image_base) {
                    p6_vtbl_rva = (uintptr_t)p6_data[0] - g_image_base;
                }
            }
            if (words[7] != 0 &&
                !IsBadReadPtr((const void *)(uintptr_t)words[7], sizeof(p7_data))) {
                memcpy(p7_data, (const void *)(uintptr_t)words[7], sizeof(p7_data));
                if (g_image_base != 0 && (uintptr_t)p7_data[0] > g_image_base) {
                    p7_vtbl_rva = (uintptr_t)p7_data[0] - g_image_base;
                }
            }
        }
        log_line(
            "[show] tid=%lu caller_rva=0x%llx r=%d vtbl_rva=0x%llx p=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx] p6v=0x%llx p6=[0x%llx,0x%llx,0x%llx,0x%llx] p7v=0x%llx p7=[0x%llx,0x%llx,0x%llx,0x%llx]",
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)caller_rva,
            readable ? 1 : 0,
            (unsigned long long)vtbl_rva,
            (unsigned long long)words[0],
            (unsigned long long)words[1],
            (unsigned long long)words[2],
            (unsigned long long)words[3],
            (unsigned long long)words[4],
            (unsigned long long)words[5],
            (unsigned long long)words[6],
            (unsigned long long)words[7],
            (unsigned long long)p6_vtbl_rva,
            (unsigned long long)p6_data[0],
            (unsigned long long)p6_data[1],
            (unsigned long long)p6_data[2],
            (unsigned long long)p6_data[3],
            (unsigned long long)p7_vtbl_rva,
            (unsigned long long)p7_data[0],
            (unsigned long long)p7_data[1],
            (unsigned long long)p7_data[2],
            (unsigned long long)p7_data[3]);
    }

    /* v3.16: reverted 0x38598b blanket mute — p7 is the subtitle renderer
     * singleton (shared by dollman/Sam/NPC gameplay paths). Speaker identity
     * lives in StartTalkFunction::a1[3]+376+0 (line_data->vtbl). Captured by
     * the new talk dispatcher probe below (hook_talk_dispatcher at 0x3857E0). */

    /* v3.17: gameplay-only Dollman filter. Match on (speaker tag, caller RVA).
     * gameplay chatter  -> speaker 0x12b6f + caller 0x38598b -> return 0
     * cutscene dialog   -> speaker 0x12b6f + caller 0x202f16f -> pass (留 cutscene)
     * anonymous dollman -> speaker 0x12b70 -> pass (cutscene 里未命名占位)
     * 其它 speaker 一律放行。payload 读前先 IsBadReadPtr。 */
    if (g_cfg.enabled && g_hotkey_throw_recall_mute &&
        caller_rva == k_dollman_gameplay_caller_rva &&
        payload != NULL && !IsBadReadPtr(payload, sizeof(uint64_t) * 8)) {
        uint64_t p7 = payload[7];
        if (p7 != 0 && !IsBadReadPtr((const void *)(uintptr_t)p7, 0x10)) {
            uint64_t w1 = *(const uint64_t *)((uintptr_t)p7 + 0x08);
            uint32_t speaker_tag = (uint32_t)(w1 >> 32);
            if (speaker_tag == k_dollman_gameplay_speaker_tag) {
                log_verbose(
                    "[mute] gameplay dollman speaker=0x%x caller=0x%llx",
                    speaker_tag,
                    (unsigned long long)caller_rva);
                return 0;
            }
        }
    }

    /* legacy TLS family path (kept as fallback in case producer still sets it) */
    if (g_cfg.enabled && muted_family != SUBTITLE_FAMILY_NONE) {
        log_verbose(
            "Muted ShowSubtitle family=%s payload=%p",
            subtitle_family_name((uint32_t)muted_family),
            (const void *)payload);
        return 0;
    }

    return g_real_show_subtitle(view, payload);
}

static uintptr_t safe_deref_qword(uintptr_t addr)
{
    if (addr == 0) {
        return 0;
    }
    if (IsBadReadPtr((const void *)addr, sizeof(uintptr_t))) {
        return 0;
    }
    return *(uintptr_t *)addr;
}

static BOOL read_localized_text_resource(
    uintptr_t ptr,
    uint32_t *tag_out,
    char *text_buffer,
    size_t text_buffer_size)
{
    uintptr_t vtbl;
    uint64_t word1;
    uintptr_t text_ptr;
    uint64_t text_len;
    size_t copy_len;

    if (tag_out != NULL) {
        *tag_out = 0;
    }
    if (text_buffer != NULL && text_buffer_size > 0) {
        text_buffer[0] = '\0';
    }

    if (ptr == 0 || IsBadReadPtr((const void *)ptr, 0x30)) {
        return FALSE;
    }

    vtbl = *(const uintptr_t *)(ptr + 0x0);
    if (g_image_base == 0 || vtbl <= g_image_base ||
        (vtbl - g_image_base) != k_rva_localized_text_resource_vtbl) {
        return FALSE;
    }

    word1 = *(const uint64_t *)(ptr + 0x8);
    if (tag_out != NULL) {
        *tag_out = (uint32_t)(word1 >> 32);
    }

    if (text_buffer == NULL || text_buffer_size == 0) {
        return TRUE;
    }

    text_ptr = *(const uintptr_t *)(ptr + 0x20);
    text_len = *(const uint64_t *)(ptr + 0x28);
    if (text_ptr == 0 || text_len == 0) {
        return TRUE;
    }

    if (text_len > 0x400) {
        return TRUE;
    }

    copy_len = (size_t)text_len;
    if (copy_len >= text_buffer_size) {
        copy_len = text_buffer_size - 1;
    }
    if (copy_len == 0 || IsBadReadPtr((const void *)text_ptr, copy_len)) {
        return TRUE;
    }

    memcpy(text_buffer, (const void *)text_ptr, copy_len);
    text_buffer[copy_len] = '\0';
    return TRUE;
}

static void log_builder_c_post_probe(uintptr_t rcx, uintptr_t result)
{
    uint64_t words[16];
    uintptr_t result_vtbl = 0;
    uintptr_t result_vtbl_rva = 0;
    int i;
    int hit_count = 0;
    uint32_t tags[4];
    uintptr_t ptrs[4];
    unsigned int offsets[4];
    char texts[4][160];

    if (result == 0 || IsBadReadPtr((const void *)result, sizeof(words))) {
        return;
    }

    ZeroMemory(words, sizeof(words));
    ZeroMemory(tags, sizeof(tags));
    ZeroMemory(ptrs, sizeof(ptrs));
    ZeroMemory(offsets, sizeof(offsets));
    ZeroMemory(texts, sizeof(texts));

    memcpy(words, (const void *)result, sizeof(words));
    result_vtbl = (uintptr_t)words[0];
    if (g_image_base != 0 && result_vtbl > g_image_base) {
        result_vtbl_rva = result_vtbl - g_image_base;
    }

    for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])) && hit_count < 4; ++i) {
        uint32_t tag = 0;
        char text[160];
        ZeroMemory(text, sizeof(text));
        if (read_localized_text_resource((uintptr_t)words[i], &tag, text, sizeof(text))) {
            tags[hit_count] = tag;
            ptrs[hit_count] = (uintptr_t)words[i];
            offsets[hit_count] = (unsigned int)(i * sizeof(uint64_t));
            snprintf(texts[hit_count], sizeof(texts[hit_count]), "%s", text);
            ++hit_count;
        }
    }

    if (hit_count == 0) {
        return;
    }

    log_line(
        "[builder-post=C] tid=%lu rcx=0x%llx result=0x%llx result_vtbl_rva=0x%llx hits=%d "
        "h0={off=0x%x ptr=0x%llx tag=0x%x text=\"%s\"} "
        "h1={off=0x%x ptr=0x%llx tag=0x%x text=\"%s\"} "
        "h2={off=0x%x ptr=0x%llx tag=0x%x text=\"%s\"} "
        "h3={off=0x%x ptr=0x%llx tag=0x%x text=\"%s\"}",
        (unsigned long)GetCurrentThreadId(),
        (unsigned long long)rcx,
        (unsigned long long)result,
        (unsigned long long)result_vtbl_rva,
        hit_count,
        offsets[0], (unsigned long long)ptrs[0], tags[0], texts[0],
        offsets[1], (unsigned long long)ptrs[1], tags[1], texts[1],
        offsets[2], (unsigned long long)ptrs[2], tags[2], texts[2],
        offsets[3], (unsigned long long)ptrs[3], tags[3], texts[3]);
}

static void log_start_talk_function_snapshot(const char *phase, uintptr_t this_obj)
{
    uintptr_t tf_vtbl;
    uintptr_t tf_vtbl_rva = 0;
    uintptr_t q1;
    uintptr_t q2;
    uintptr_t sh2_vtbl;
    uintptr_t sh2_vtbl_rva = 0;
    uintptr_t p200;
    uintptr_t p200_deref;
    uintptr_t field72;
    uint32_t hi32 = 0;
    uint32_t tag1 = 0;
    uint32_t tag2 = 0;
    char text1[160];
    char text2[160];
    BOOL loc1;
    BOOL loc2;

    if (this_obj == 0 || IsBadReadPtr((const void *)this_obj, 0xD0)) {
        return;
    }

    ZeroMemory(text1, sizeof(text1));
    ZeroMemory(text2, sizeof(text2));

    tf_vtbl = safe_deref_qword(this_obj + 0x0);
    if (g_image_base != 0 && tf_vtbl > g_image_base) {
        tf_vtbl_rva = tf_vtbl - g_image_base;
    }

    q1 = safe_deref_qword(this_obj + 0x8);
    q2 = safe_deref_qword(this_obj + 0x10);
    loc1 = read_localized_text_resource(q1, &tag1, text1, sizeof(text1));
    loc2 = read_localized_text_resource(q2, &tag2, text2, sizeof(text2));

    sh2_vtbl = safe_deref_qword(this_obj + 0x88);
    if (g_image_base != 0 && sh2_vtbl > g_image_base) {
        sh2_vtbl_rva = sh2_vtbl - g_image_base;
    }

    p200 = safe_deref_qword(this_obj + 200);
    p200_deref = safe_deref_qword(p200);
    field72 = safe_deref_qword(p200_deref + 72);
    hi32 = read_identity_hi32(field72);

    log_line(
        "[stf-%s] tid=%lu this=0x%llx tf_vtbl_rva=0x%llx "
        "q1=0x%llx loc1=%d tag1=0x%x text1=\"%s\" "
        "q2=0x%llx loc2=%d tag2=0x%x text2=\"%s\" "
        "sh2_vtbl_rva=0x%llx p200=0x%llx field72=0x%llx hi32=0x%x",
        phase != NULL ? phase : "?",
        (unsigned long)GetCurrentThreadId(),
        (unsigned long long)this_obj,
        (unsigned long long)tf_vtbl_rva,
        (unsigned long long)q1,
        loc1 ? 1 : 0,
        (unsigned int)tag1,
        text1,
        (unsigned long long)q2,
        loc2 ? 1 : 0,
        (unsigned int)tag2,
        text2,
        (unsigned long long)sh2_vtbl_rva,
        (unsigned long long)p200,
        (unsigned long long)field72,
        (unsigned int)hi32);
}

static BOOL identity_seen_or_mark(uintptr_t key)
{
    int i;
    BOOL already = FALSE;
    BOOL warn_full = FALSE;

    if (key == 0) {
        return TRUE;
    }

    EnterCriticalSection(&g_identity_lock);
    for (i = 0; i < g_identity_cache_count; ++i) {
        if (g_identity_cache[i] == key) {
            already = TRUE;
            break;
        }
    }
    if (!already) {
        if (g_identity_cache_count < PRODUCER_IDENTITY_CACHE_MAX) {
            g_identity_cache[g_identity_cache_count++] = key;
        } else {
            already = TRUE;
            if (!g_identity_cache_full_warned) {
                g_identity_cache_full_warned = TRUE;
                warn_full = TRUE;
            }
        }
    }
    LeaveCriticalSection(&g_identity_lock);

    if (warn_full) {
        log_line("ProducerIdentity cache full (%d slots) - further new ptrs suppressed",
                 PRODUCER_IDENTITY_CACHE_MAX);
    }

    return already;
}

static void log_producer_identity(const char *tag, int slot, uintptr_t ptr)
{
    uint64_t words[8];
    uintptr_t vtbl = 0;
    uintptr_t vtbl_rva = 0;

    if (ptr == 0 || identity_seen_or_mark(ptr)) {
        return;
    }
    if (IsBadReadPtr((const void *)ptr, sizeof(words))) {
        return;
    }

    memcpy(words, (const void *)ptr, sizeof(words));
    vtbl = (uintptr_t)words[0];
    if (g_image_base != 0 && vtbl > g_image_base) {
        vtbl_rva = vtbl - g_image_base;
    }

    log_line(
        "ProducerIdentity %s slot=%d ptr=0x%llx vtbl=0x%llx vtbl_rva=0x%llx words=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]",
        tag != NULL ? tag : "?",
        slot,
        (unsigned long long)ptr,
        (unsigned long long)vtbl,
        (unsigned long long)vtbl_rva,
        (unsigned long long)words[0],
        (unsigned long long)words[1],
        (unsigned long long)words[2],
        (unsigned long long)words[3],
        (unsigned long long)words[4],
        (unsigned long long)words[5],
        (unsigned long long)words[6],
        (unsigned long long)words[7]);
}

static uintptr_t __fastcall hook_subtitle_producer(uintptr_t this_obj)
{
    uintptr_t pre_p200 = 0;
    uintptr_t pre_p200_deref = 0;
    uintptr_t pre_p200_field72 = 0;
    uintptr_t previous_muted_family = SUBTITLE_FAMILY_NONE;
    uint32_t family = SUBTITLE_FAMILY_NONE;
    BOOL mute_this_call = FALSE;
    uintptr_t result;
    BOOL probe_enabled = (g_cfg.enable_builder_probe || g_cfg.enable_selector_probe);

    if (probe_enabled) {
        log_start_talk_function_snapshot("pre", this_obj);
    }

    if (this_obj != 0) {
        pre_p200 = safe_deref_qword(this_obj + 200);
        pre_p200_deref = safe_deref_qword(pre_p200);
        pre_p200_field72 = safe_deref_qword(pre_p200_deref + 72);
        family = classify_subtitle_family(pre_p200_field72);
        if (g_cfg.enabled && should_mute_subtitle_family(family)) {
            previous_muted_family = tls_get_muted_subtitle_family();
            tls_set_muted_subtitle_family((uintptr_t)family);
            mute_this_call = TRUE;
        }
    }

    result = g_real_subtitle_producer(this_obj);

    if (probe_enabled) {
        log_start_talk_function_snapshot("post", this_obj);
    }

    if (mute_this_call) {
        tls_set_muted_subtitle_family(previous_muted_family);
    }

    return result;
}

static uintptr_t builder_common(
    uint32_t id,
    void *real_fn,
    uintptr_t rcx,
    uintptr_t rdx,
    uintptr_t r8,
    uintptr_t r9,
    uintptr_t caller_ra)
{
    uint32_t prev;
    LONG hits;
    uintptr_t result;
    uintptr_t caller_rva;
    uint64_t msg_words[8];
    uintptr_t msg_vtbl = 0;
    uintptr_t msg_vtbl_rva = 0;
    BOOL msg_readable = FALSE;

    prev = tls_get_last_builder();
    tls_set_last_builder(id);
    hits = InterlockedIncrement(&g_builder_hit_counts[id]);

    caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                     ? (caller_ra - g_image_base)
                     : 0;

    ZeroMemory(msg_words, sizeof(msg_words));
    if (rdx != 0 && !IsBadReadPtr((const void *)rdx, sizeof(msg_words))) {
        memcpy(msg_words, (const void *)rdx, sizeof(msg_words));
        msg_vtbl = (uintptr_t)msg_words[0];
        if (g_image_base != 0 && msg_vtbl > g_image_base) {
            msg_vtbl_rva = msg_vtbl - g_image_base;
        }
        msg_readable = TRUE;
    }

    if (g_cfg.enable_builder_probe) {
        const char *klass = (id == BUILDER_ID_C)
            ? classify_builder_c_msg(msg_vtbl_rva, msg_words[1], msg_words[2], msg_words[3])
            : "n/a";
        log_line(
            "[builder=%s] tid=%lu this=0x%llx arg2=0x%llx caller_rva=0x%llx hits=%ld msg_vtbl_rva=0x%llx msg_readable=%d class=%s msg=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]",
            k_builder_names[id],
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)rcx,
            (unsigned long long)rdx,
            (unsigned long long)caller_rva,
            (long)hits,
            (unsigned long long)msg_vtbl_rva,
            msg_readable ? 1 : 0,
            klass,
            (unsigned long long)msg_words[0],
            (unsigned long long)msg_words[1],
            (unsigned long long)msg_words[2],
            (unsigned long long)msg_words[3],
            (unsigned long long)msg_words[4],
            (unsigned long long)msg_words[5],
            (unsigned long long)msg_words[6],
            (unsigned long long)msg_words[7]);
    }

    result = ((BuilderFn)real_fn)(rcx, rdx, r8, r9);

    if ((g_cfg.enable_builder_probe || g_cfg.enable_selector_probe) &&
        id == BUILDER_ID_C) {
        log_builder_c_post_probe(rcx, result);
    }

    tls_set_last_builder(prev);
    return result;
}

static uintptr_t __fastcall hook_builder_a(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
{
    return builder_common(BUILDER_ID_A, g_real_builder_a, rcx, rdx, r8, r9,
                          (uintptr_t)__builtin_return_address(0));
}

static uintptr_t __fastcall hook_builder_b(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
{
    return builder_common(BUILDER_ID_B, g_real_builder_b, rcx, rdx, r8, r9,
                          (uintptr_t)__builtin_return_address(0));
}

static uintptr_t __fastcall hook_builder_c(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
{
    return builder_common(BUILDER_ID_C, g_real_builder_c, rcx, rdx, r8, r9,
                          (uintptr_t)__builtin_return_address(0));
}

static uintptr_t __fastcall hook_builder_u1(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
{
    return builder_common(BUILDER_ID_U1, g_real_builder_u1, rcx, rdx, r8, r9,
                          (uintptr_t)__builtin_return_address(0));
}

static uintptr_t __fastcall hook_builder_u2(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
{
    return builder_common(BUILDER_ID_U2, g_real_builder_u2, rcx, rdx, r8, r9,
                          (uintptr_t)__builtin_return_address(0));
}

static uintptr_t __fastcall hook_selector_dispatch(uintptr_t rcx, uintptr_t rdx)
{
    uintptr_t caller_ra = (uintptr_t)__builtin_return_address(0);

    if (g_cfg.enable_selector_probe) {
        uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                                   ? (caller_ra - g_image_base)
                                   : 0;
        uint8_t selector = (uint8_t)(rdx & 0xFFu);
        uint32_t pending = 0xFFFFFFFFu;
        uintptr_t ref500 = 0;
        int32_t count = -1;

        if (rcx != 0) {
            if (!IsBadReadPtr((const void *)(rcx + 0x74), sizeof(uint8_t))) {
                pending = *(volatile const uint8_t *)(rcx + 0x74);
            }
            if (!IsBadReadPtr((const void *)(rcx + 0x500), sizeof(uintptr_t))) {
                ref500 = *(volatile const uintptr_t *)(rcx + 0x500);
            }
            if (!IsBadReadPtr((const void *)(rcx + 0x30), sizeof(uintptr_t))) {
                uintptr_t p30 = *(volatile const uintptr_t *)(rcx + 0x30);
                if (p30 != 0 &&
                    !IsBadReadPtr((const void *)(p30 + 0x28), sizeof(uintptr_t))) {
                    uintptr_t table = *(volatile const uintptr_t *)(p30 + 0x28);
                    if (table != 0) {
                        uintptr_t entry = table + (uintptr_t)selector * 0x18u;
                        if (!IsBadReadPtr((const void *)(entry + 8), sizeof(int32_t))) {
                            count = *(volatile const int32_t *)(entry + 8);
                        }
                    }
                }
            }
        }

        log_line(
            "[dafad0] tid=%lu this=0x%llx sel=0x%02x pending=0x%02x ref500=0x%llx count=%d caller_rva=0x%llx",
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)rcx,
            (unsigned int)selector,
            (unsigned int)pending,
            (unsigned long long)ref500,
            (int)count,
            (unsigned long long)caller_rva);
    }

    return g_real_selector_dispatch(rcx, rdx);
}

__declspec(dllexport) int core_init(const ProxyContext *ctx)
{
    MH_STATUS status;
    unsigned int hook_count = 0;
    BOOL throw_recall_mute = FALSE;
    BOOL dialogue_mute = FALSE;

    ZeroMemory(&g_proxy_ctx, sizeof(g_proxy_ctx));
    if (ctx != NULL) {
        g_proxy_ctx = *ctx;
    }

    InterlockedExchange(&g_core_shutting_down, 0);

    InitializeCriticalSection(&g_log_lock);
    g_log_lock_inited = TRUE;
    InitializeCriticalSection(&g_identity_lock);
    g_identity_lock_inited = TRUE;
    InitializeCriticalSection(&g_hotkey_lock);
    g_hotkey_lock_inited = TRUE;

    init_paths();
    load_config();

    g_image_base = (uintptr_t)GetModuleHandleW(NULL);
    if (g_image_base != 0) {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)g_image_base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(g_image_base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                g_image_size = nt->OptionalHeader.SizeOfImage;
            }
        }
    }
    g_tls_muted_subtitle_family = TlsAlloc();
    if (g_tls_muted_subtitle_family == TLS_OUT_OF_INDEXES) {
        log_line("TlsAlloc failed: %lu", (unsigned long)GetLastError());
    }
    g_tls_last_builder = TlsAlloc();
    if (g_tls_last_builder == TLS_OUT_OF_INDEXES) {
        log_line("TlsAlloc(last_builder) failed: %lu", (unsigned long)GetLastError());
    }
    {
        size_t bi;
        for (bi = 0; bi < BUILDER_ID_COUNT; ++bi) {
            InterlockedExchange(&g_builder_hit_counts[bi], 0);
        }
    }
    InterlockedExchange(&g_session_counter, 0);
    g_hotkey_f8_prev = FALSE;
    seed_hotkey_state_from_config();
    get_hotkey_mute_state(&throw_recall_mute, &dialogue_mute);

    log_line("DollmanMute build: %s", k_build_tag);
    log_line("DollmanMute image_base=0x%llx image_size=0x%llx", (unsigned long long)g_image_base, (unsigned long long)g_image_size);
    log_line(
        "DollmanMute init start: enabled=%d verbose=%d dollmanRadioMute=%d throwRecallSubtitleMute=%d producerProbe=%d builderProbe=%d scannerMode=%u",
        g_cfg.enabled,
        g_cfg.verbose_log,
        g_cfg.enable_dollman_radio_mute,
        g_cfg.enable_throw_recall_subtitle_mute,
        g_cfg.enable_subtitle_producer_probe,
        g_cfg.enable_builder_probe,
        (unsigned int)g_cfg.scanner_mode);

    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("MH_Initialize failed: %d", (int)status);
        return 0;
    }

    if (g_cfg.enable_dollman_radio_mute || g_cfg.scanner_mode != SCANNER_MODE_OFF) {
        if (install_export_hook(
                k_export_post_event_id,
                hook_post_event_id,
                (void **)&g_real_post_event_id,
                "PostEventID")) {
            ++hook_count;
        }
    } else {
        log_line("Legacy PostEvent mute path disabled");
    }

    if (g_cfg.enable_dollman_radio_mute) {
        if (install_rva_hook(
                k_rva_dollman_radio_play_voice_by_controller_delay,
                hook_dollman_radio_play_voice_by_controller_delay,
                (void **)&g_real_dollman_radio_play_voice_by_controller_delay,
                "DollmanRadio.PlayVoiceByControllerDelay")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_dollman_voice_dispatcher,
                hook_dollman_voice_dispatcher,
                (void **)&g_real_dollman_voice_dispatcher,
                "DollmanVoiceDispatcher")) {
            ++hook_count;
        }
    } else {
        log_line("Dollman radio mute disabled by config");
    }

    if (install_rva_hook(
            k_rva_show_subtitle,
            hook_show_subtitle,
            (void **)&g_real_show_subtitle,
            "GameViewGame.ShowSubtitle")) {
        ++hook_count;
    }

    if (install_rva_hook(
            k_rva_subtitle_producer,
            hook_subtitle_producer,
            (void **)&g_real_subtitle_producer,
            "StartTalkFunction.slot15.producer")) {
        ++hook_count;
    }

    if (g_cfg.enable_builder_probe) {
        if (install_rva_hook(
                k_rva_builder_a,
                hook_builder_a,
                (void **)&g_real_builder_a,
                "BuilderA.sub_140350050")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_b,
                hook_builder_b,
                (void **)&g_real_builder_b,
                "BuilderB.sub_140350460")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_c,
                hook_builder_c,
                (void **)&g_real_builder_c,
                "BuilderC.sub_140350960")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_u1,
                hook_builder_u1,
                (void **)&g_real_builder_u1,
                "BuilderU1.sub_140B5BC70")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_u2,
                hook_builder_u2,
                (void **)&g_real_builder_u2,
                "BuilderU2.sub_140B5CC30")) {
            ++hook_count;
        }
        log_line("Builder probe armed: F8 = session boundary marker");
    } else {
        log_line("Builder probe disabled by config");
    }

    if (g_cfg.enable_selector_probe) {
        if (install_rva_hook(
                k_rva_selector_dispatch,
                hook_selector_dispatch,
                (void **)&g_real_selector_dispatch,
                "SelectorDispatch.sub_140DAFAD0")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_talk_dispatcher,
                hook_talk_dispatcher,
                (void **)&g_real_talk_dispatcher,
                "TalkDispatcher.sub_1403857E0")) {
            ++hook_count;
        }
        log_line("Selector dispatch probe armed (sub_140DAFAD0 + sub_1403857E0)");
    } else {
        log_line("Selector dispatch probe disabled by config");
    }

    g_hotkey_thread_handle = CreateThread(NULL, 0, hotkey_thread_proc, NULL, 0, NULL);
    if (g_hotkey_thread_handle != NULL) {
        log_line(
            "Hotkeys active: J=throwRecall on, K=throwRecall off, N=dialogue on, M=dialogue off, F12=clear (startup throwRecall=%d dialogue=%d)",
            throw_recall_mute ? 1 : 0,
            dialogue_mute ? 1 : 0);
    } else {
        log_line("Failed to start hotkey thread");
    }

    log_line("DollmanMute init complete: hooks=%u", hook_count);

    return 0;
}

__declspec(dllexport) void core_shutdown(void)
{
    DWORD wait_result;

    log_line("core_shutdown begin");

    InterlockedExchange(&g_core_shutting_down, 1);

    if (g_hotkey_thread_handle != NULL) {
        wait_result = WaitForSingleObject(g_hotkey_thread_handle, 500);
        if (wait_result != WAIT_OBJECT_0) {
            log_line("core_shutdown: hotkey thread did not exit in 500ms, terminating");
            TerminateThread(g_hotkey_thread_handle, 0);
        }
        CloseHandle(g_hotkey_thread_handle);
        g_hotkey_thread_handle = NULL;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    log_line(
        "Builder probe hit summary: A=%ld B=%ld C=%ld U1=%ld U2=%ld sessions=%ld",
        (long)g_builder_hit_counts[BUILDER_ID_A],
        (long)g_builder_hit_counts[BUILDER_ID_B],
        (long)g_builder_hit_counts[BUILDER_ID_C],
        (long)g_builder_hit_counts[BUILDER_ID_U1],
        (long)g_builder_hit_counts[BUILDER_ID_U2],
        (long)g_session_counter);

    g_real_post_event_id = NULL;
    g_real_dollman_radio_play_voice_by_controller_delay = NULL;
    g_real_dollman_voice_dispatcher = NULL;
    g_real_show_subtitle = NULL;
    g_real_subtitle_producer = NULL;
    g_real_builder_a = NULL;
    g_real_builder_b = NULL;
    g_real_builder_c = NULL;
    g_real_builder_u1 = NULL;
    g_real_builder_u2 = NULL;
    g_real_selector_dispatch = NULL;
    g_real_talk_dispatcher = NULL;

    if (g_tls_muted_subtitle_family != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_muted_subtitle_family);
        g_tls_muted_subtitle_family = TLS_OUT_OF_INDEXES;
    }
    if (g_tls_last_builder != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_last_builder);
        g_tls_last_builder = TLS_OUT_OF_INDEXES;
    }

    g_identity_cache_count = 0;
    g_identity_cache_full_warned = FALSE;
    g_image_base = 0;

    g_hotkey_j_prev = FALSE;
    g_hotkey_k_prev = FALSE;
    g_hotkey_n_prev = FALSE;
    g_hotkey_m_prev = FALSE;
    g_hotkey_f12_prev = FALSE;
    g_hotkey_f8_prev = FALSE;
    g_hotkey_f9_prev = FALSE;
    InterlockedExchange(&g_session_counter, 0);

    log_line("core_shutdown complete");

    if (g_hotkey_lock_inited) {
        DeleteCriticalSection(&g_hotkey_lock);
        g_hotkey_lock_inited = FALSE;
    }
    if (g_identity_lock_inited) {
        DeleteCriticalSection(&g_identity_lock);
        g_identity_lock_inited = FALSE;
    }
    if (g_log_lock_inited) {
        DeleteCriticalSection(&g_log_lock);
        g_log_lock_inited = FALSE;
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

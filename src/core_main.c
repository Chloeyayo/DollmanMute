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
    uint32_t active_subtitle_strategy;
    BOOL enable_sender_only_runtime_mode;
    BOOL enable_subtitle_runtime_hooks;
    BOOL enable_subtitle_family_tracking;
    BOOL enable_subtitle_producer_probe;
    BOOL enable_builder_probe;
    BOOL enable_selector_probe;
    BOOL enable_deep_probe;
    BOOL enable_talk_dispatcher_probe;
    BOOL enable_legacy_runtime_wrapper;
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
typedef int(__fastcall *SoundInstanceSubmitFn)(
    AkUniqueID event_id,
    uintptr_t sound_instance,
    uint8_t a3,
    uint8_t a4,
    uintptr_t s0,
    uintptr_t s1,
    uintptr_t s2,
    uintptr_t s3,
    uintptr_t s4,
    uintptr_t s5,
    uintptr_t s6,
    uintptr_t s7,
    uintptr_t s8,
    uintptr_t s9);
typedef uintptr_t(__fastcall *StartTalkGetOrCreateSoundWrapperFn)(
    uintptr_t starttalk,
    uintptr_t out_wrapper);
typedef uintptr_t(__fastcall *ShowSubtitleFn)(uintptr_t view, const uint64_t *payload);
typedef uintptr_t(__fastcall *RemoveSubtitleFn)(uintptr_t view, const uint64_t *key_pair, char mode);

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
static SoundInstanceSubmitFn g_real_sound_instance_submit = NULL;
static StartTalkGetOrCreateSoundWrapperFn g_real_start_talk_get_or_create_sound_wrapper = NULL;
static ShowSubtitleFn g_real_show_subtitle = NULL;
static RemoveSubtitleFn g_real_remove_subtitle = NULL;
static void **g_show_subtitle_vtable_slot = NULL;
static void *g_show_subtitle_vtable_original = NULL;

static const char *k_build_tag = "v2.1.18-v1.8-starttalk-dev";

#define PRODUCER_IDENTITY_CACHE_MAX 4096
static uintptr_t g_image_base = 0;
static uintptr_t g_image_size = 0;
static const uintptr_t k_rva_localized_text_resource_vtbl = 0x03455BD0u;

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
static CRITICAL_SECTION g_stf_probe_lock;
static BOOL g_stf_probe_lock_inited = FALSE;
#define STF_PROBE_CACHE_MAX 256
static uint64_t g_stf_probe_cache[STF_PROBE_CACHE_MAX];
static int g_stf_probe_cache_count = 0;
#define DOLLMAN_STARTTALK_SOUND_CACHE_MAX 128
#define DOLLMAN_STARTTALK_CACHE_TTL_MS 10000ull
typedef struct DollmanStartTalkSoundMark {
    uintptr_t sound;
    uintptr_t line;
    uintptr_t voice;
    BOOL should_mute;
    ULONGLONG seen_ms;
} DollmanStartTalkSoundMark;
typedef struct DollmanStartTalkInstanceMark {
    uintptr_t sound_instance;
    uintptr_t sound_resource;
    BOOL should_mute;
    ULONGLONG seen_ms;
} DollmanStartTalkInstanceMark;
static DollmanStartTalkSoundMark g_dollman_starttalk_sounds[DOLLMAN_STARTTALK_SOUND_CACHE_MAX];
static DollmanStartTalkInstanceMark g_dollman_starttalk_instances[DOLLMAN_STARTTALK_SOUND_CACHE_MAX];
static volatile LONG g_dollman_starttalk_sound_cursor = 0;
static volatile LONG g_dollman_starttalk_instance_cursor = 0;
static volatile LONG g_starttalk_bridge_sample_hits = 0;
static volatile LONG64 g_stf_probe_window_until_ms = 0;
static CRITICAL_SECTION g_hotkey_lock;
static BOOL g_hotkey_lock_inited = FALSE;
static DWORD g_tls_current_subtitle_family = TLS_OUT_OF_INDEXES;
static volatile LONG g_subtitle_runtime_hits = 0;
static volatile LONG g_subtitle_remove_hits = 0;
static volatile LONG64 g_last_dowser_subtitle_ms = 0;
static uint64_t g_last_dowser_key2 = 0;
static uint64_t g_last_dowser_key3 = 0;
static uintptr_t g_last_dowser_p6 = 0;
static uintptr_t g_last_dowser_p7 = 0;
static volatile LONG64 g_last_dollman_muted_subtitle_ms = 0;
static uint32_t g_last_dollman_muted_speaker_tag = 0;
static uint32_t g_last_dollman_muted_line_tag = 0;
static uintptr_t g_last_dollman_muted_caller_rva = 0;

static const char *k_export_post_event_id =
    "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";

/* Legacy broad audio hook names are kept for source continuity. On the current
 * v1.5 build, 0x00C73BF0 landed in a ThroughDollmanInstance teardown path, not
 * the live delay scheduler. For v1.6, the live Dollman delay scheduler is
 * 0x00C73E80 and the Dollman-only runtime voice closure is 0x00C73F30.
 * The old 0x00DAA410 "dispatcher" probe was a manager tick/update; the real
 * shared voice submit helper is 0x00DACCD0 in v1.6.
 * On v1.6 the Player-side path that calls the shared helper has been
 * refactored: it no longer flows through the v1.5 player closure (sub_140C73A60)
 * but through sub_140C743B0, where the call to sub_140DACCD0 sits at
 * 0x140C74438 (return RVA 0x00C7443D). The Dollman-side path still calls the
 * helper from sub_140C73F30; the call sits at 0x140C73FB9 (return 0x00C73FBE).
 * Verified by IDA xrefs to sub_140DACCD0 on the v1.6 image. */
static const uintptr_t k_rva_dollman_voice_delay_schedule = 0x00C78E10u;
static const uintptr_t k_rva_dollman_voice_delay_closure = 0x00C78EC0u;
static const uintptr_t k_rva_voice_shared_helper = 0x00DB4870u;
static const uintptr_t k_rva_voice_shared_helper_player_return = 0x00C793CDu;
static const uintptr_t k_rva_voice_shared_helper_dollman_return = 0x00C78F4Eu;
static const uintptr_t k_rva_voice_queue_submit = 0x00DB49D0u;
static const uintptr_t k_rva_voice_queue_shared_helper_return = 0x00DB4951u;
static const uintptr_t k_rva_voice_queue_dispatcher_synth_return = 0x00DB2C24u;
static const uintptr_t k_rva_voice_queue_dispatcher_forward_return = 0x00DB423Au;
static const uintptr_t k_rva_sound_instance_play = 0x026A72E0u;
static const uintptr_t k_rva_sound_instance_submit = 0x026C1F60u;
static const uintptr_t k_rva_audio_owner_get_variant_resource = 0x0028EA30u;
static const uintptr_t k_rva_start_talk_get_or_create_sound_wrapper = 0x00388280u;
static const uintptr_t k_rva_subtitle_runtime_wrapper = 0x00780F10u;
static const uintptr_t k_rva_show_subtitle = 0x00780FC0u;
static const uintptr_t k_rva_remove_subtitle = 0x007810C0u;
static const uintptr_t k_rva_subtitle_render = 0x00781120u;
static const uintptr_t k_rva_subtitle_prepare = 0x0025ACC0u;
static const uintptr_t k_rva_subtitle_runtime_context = 0x0623C0B8u;
static const uintptr_t k_rva_game_view_game_show_subtitle_slot = 0x0A45E088u;
static const uintptr_t k_rva_subtitle_producer = 0x003875D0u;
static const uintptr_t k_rva_start_talk_init = 0x00387980u;
static const uintptr_t k_rva_selector_dispatch = 0x00DB7960u;
static const uintptr_t k_rva_talk_dispatcher = 0x00385A30u;
static const uintptr_t k_rva_gameplay_sink = 0u;

/* Current build gameplay Dollman mute: observed (speaker tag, ShowSubtitle
 * caller RVA) pair for the chatter path. v1.6 live sender now lands at
 * 0x385C5B. */
static const uint32_t k_dollman_gameplay_speaker_tag = 0x12b72u;
static const uintptr_t k_dollman_gameplay_caller_rva = 0x385f2bu;
static const uint32_t k_dowser_gameplay_speaker_tag = 0x0e406u;
static const uint32_t k_dowser_gameplay_line_tag = 0x0e404u;

static const AkUniqueID k_scanner_event_id_1 = 4235852663u;
static const AkUniqueID k_scanner_event_id_2 = 4094913469u;
static const AkUniqueID k_scanner_event_id_3 = 2611919341u;
static const AkUniqueID k_event_id_dowser_gameplay_chatter = 2134002697u;
static const uint64_t k_dowser_ext0_sample = 0x47f324c09ull;
static const AkUniqueID k_event_id_dollman_fall_chatter = 448888368u;
static const uint64_t k_dollman_fall_chatter_ext0_sample = 0x41ac17e30ull;

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

enum {
    SUBTITLE_STRATEGY_OBSERVE = 0u,
    SUBTITLE_STRATEGY_GAMEPLAY_PAIR = 1u,
    SUBTITLE_STRATEGY_CALLER_ONLY = 2u,
    SUBTITLE_STRATEGY_SPEAKER_ONLY = 3u,
    SUBTITLE_STRATEGY_SELECTED_FAMILY = 4u,
    SUBTITLE_STRATEGY_PAIR_OR_SELECTED_FAMILY = 5u,
    SUBTITLE_STRATEGY_COUNT = 6u
};

enum {
    HOTKEY_CONTROL_SESSION_MARK = 0u,
    HOTKEY_CONTROL_COUNT = 1u
};

typedef struct ShowStrategyContext {
    uintptr_t caller_rva;
    uint32_t current_family;
    uint32_t speaker_tag;
    BOOL speaker_tag_valid;
    uint32_t last_builder;
} ShowStrategyContext;

typedef struct SubtitleStrategyStats {
    volatile LONG evaluated;
    volatile LONG would_mute;
    volatile LONG actual_mute;
} SubtitleStrategyStats;

typedef struct SubtitleStrategyMeta {
    const char *name;
    const char *desc;
    int vk;
} SubtitleStrategyMeta;

static const uint32_t k_identity_tag_throw_recall = 0x01F4u;
static const uint32_t k_identity_tag_dialogue_a = 0x222Cu;
static const uint32_t k_identity_tag_dialogue_b = 0x4377u;
static const SubtitleStrategyMeta k_subtitle_strategy_meta[SUBTITLE_STRATEGY_COUNT] = {
    { "observe", "never mute; log-only baseline", VK_F1 },
    { "pair", "mute only when caller 0x385F2B and speaker tag 0x12B72 both match", VK_F2 },
    { "callerOnly", "mute everything from caller 0x385F2B", VK_F3 },
    { "speakerOnly", "mute everything with speaker tag 0x12B72", VK_F4 },
    { "selectedFamily", "mute only the subtitle families enabled by config defaults", VK_F5 },
    { "pairOrSelectedFamily", "mute when the gameplay pair matches or the config-selected family matches", VK_F6 }
};
static const int k_hotkey_control_vks[HOTKEY_CONTROL_COUNT] = { VK_F8 };
static const AkUniqueID k_event_id_dollman_equip = 2995625663u;
static const AkUniqueID k_event_id_dollman_throw = 2820786646u;
static const AkUniqueID k_event_id_dollman_recall = 2978848044u;

/* --- Builder probe (v3.14): log-only hooks on gameplay subtitle builders --- */
enum {
    BUILDER_ID_NONE = 0u,
    BUILDER_ID_A    = 1u, /* current build: unresolved, quarantined */
    BUILDER_ID_B    = 2u, /* sub_1403503C0 — MsgStartTalk handler */
    BUILDER_ID_C    = 3u, /* sub_1403507D0 — MsgDSStartTalk handler */
    BUILDER_ID_U1   = 4u, /* sub_140B5C130 — unclassified 0x388950 caller */
    BUILDER_ID_U2   = 5u, /* sub_140B5D0F0 — unclassified 0x388950 caller */
    BUILDER_ID_COUNT = 6u
};

static const char *k_builder_names[BUILDER_ID_COUNT] = {
    "none", "A", "B", "C", "U1", "U2"
};

static const uintptr_t k_rva_builder_a  = 0u;
static const uintptr_t k_rva_builder_b  = 0x003506C0u;
static const uintptr_t k_rva_builder_c  = 0x00350AD0u;
static const uintptr_t k_rva_builder_u1 = 0x00B5CE10u;
static const uintptr_t k_rva_builder_u2 = 0x00B5DDD0u;

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
static uint32_t g_active_subtitle_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
static SubtitleStrategyStats g_strategy_stats[SUBTITLE_STRATEGY_COUNT];
static BOOL g_hotkey_control_prev[HOTKEY_CONTROL_COUNT] = {FALSE};
static volatile LONG g_session_counter = 0;

static const AkUniqueID k_blocked_event_ids[] = {
    k_event_id_dollman_equip, /* equip */
    k_event_id_dollman_throw,  /* throw */
    k_event_id_dollman_recall, /* recall */
    1966841225u, /* random chatter */
    302733266u,  /* task-failure */
    448888368u   /* fall chatter */
};

static uintptr_t safe_deref_qword(uintptr_t addr);
static uintptr_t safe_read_ptr(uintptr_t addr);
static uint64_t safe_read_u64(uintptr_t addr);
static uint32_t safe_read_u32(uintptr_t addr);
static const char *subtitle_strategy_name(uint32_t strategy);
static BOOL read_localized_text_resource(
    uintptr_t ptr,
    uint32_t *tag_out,
    char *text_buffer,
    size_t text_buffer_size);
static void log_localized_text_resource_candidate(
    const char *label,
    uintptr_t ptr);
static void log_builder_c_post_probe(uintptr_t rcx, uintptr_t result);
static void log_start_talk_function_snapshot(const char *phase, uintptr_t this_obj);
static void log_dollman_voice_closure_probe(const char *phase, uintptr_t closure_state);
static BOOL is_voice_queue_probe_caller(uintptr_t caller_rva);
static const char *voice_queue_probe_caller_name(uintptr_t caller_rva);
static uintptr_t get_return_address_value(void);
static BOOL is_stf_probe_window_open(void);
static void reset_stf_probe_cache(void);
static BOOL stf_probe_seen_or_mark(uint64_t key);
static void reset_dollman_starttalk_sound_cache(void);
static void note_dollman_starttalk_sound(uintptr_t sound, uintptr_t line, uintptr_t voice, BOOL should_mute);
static BOOL is_recent_dollman_starttalk_sound(uintptr_t ptr, BOOL should_mute, ULONGLONG now_ms, ULONGLONG *delta_ms_out);
static void note_dollman_starttalk_instance(uintptr_t sound_instance, uintptr_t sound_resource, BOOL should_mute);
static BOOL is_recent_dollman_starttalk_instance(uintptr_t ptr, BOOL should_mute, ULONGLONG now_ms, ULONGLONG *delta_ms_out);
static void reset_builder_hit_counts(void);
static void reset_strategy_stats(void);
static void reset_hotkey_runtime_state(void);
static void reset_session_probe_state(void);
static void reset_runtime_capture_counters(void);
static void reset_identity_probe_cache(void);
static void reset_log_capture_state(void);
static uint32_t get_active_subtitle_strategy(void);
static BOOL is_dowser_gameplay_subtitle(
    uintptr_t caller_rva,
    uint32_t speaker_tag,
    BOOL speaker_tag_valid,
    uint32_t line_tag,
    BOOL line_tag_valid);
static void note_dowser_gameplay_subtitle(
    uintptr_t caller_rva,
    const uint64_t *words,
    size_t word_count,
    uint32_t speaker_tag,
    uint32_t line_tag);
static BOOL get_recent_dowser_subtitle_delta_ms(
    ULONGLONG now_ms,
    ULONGLONG *delta_ms_out);
static void note_dollman_muted_subtitle(
    uintptr_t caller_rva,
    uint32_t speaker_tag,
    uint32_t line_tag);
static BOOL get_recent_dollman_muted_subtitle_delta_ms(
    ULONGLONG now_ms,
    ULONGLONG *delta_ms_out);
static void log_localized_hits_in_block(
    uintptr_t caller_rva,
    const char *label,
    uintptr_t base,
    size_t size);
static void log_sentence_desc_probe(
    const char *tag,
    uintptr_t caller_rva,
    unsigned int index,
    uintptr_t desc);
static void *resolve_rva(uintptr_t rva);
static BOOL subtitle_strategy_uses_family_tracking(uint32_t strategy);
static BOOL is_subtitle_runtime_mute_enabled(void);
static BOOL is_sender_only_runtime_mode_enabled(void);
static BOOL is_legacy_dollman_radio_mute_enabled(void);
static BOOL is_sender_only_dollman_radio_mute_enabled(void);
static BOOL should_block_sender_only_event_id(
    AkUniqueID event_id,
    uint32_t external_source_count,
    uint64_t ext0);
static uint32_t classify_subtitle_family_from_identity_tag(uint32_t identity_tag);
static uint32_t read_subtitle_runtime_prepare_token(void);
static void log_subtitle_identity_probe(
    const char *surface,
    uintptr_t caller_rva,
    const uint64_t *words,
    size_t word_count);
static void log_remove_subtitle_probe(
    uintptr_t caller_rva,
    const uint64_t *key_pair,
    char mode);
static BOOL process_subtitle_payload(
    const char *surface,
    uintptr_t caller_rva,
    const uint64_t *payload);
static uintptr_t pass_through_subtitle_runtime_wrapper(
    uintptr_t view,
    uintptr_t arg2,
    const char *reason);
static void log_pair_bypass_probe(
    uint32_t active_strategy,
    const ShowStrategyContext *ctx,
    uint32_t line_tag,
    BOOL line_tag_valid,
    BOOL pair_match,
    BOOL preamble_match);

static const char *k_default_ini =
    "; DollmanMute runtime config.\n"
    "; VerboseLog=1 enables extra logging for troubleshooting.\n"
    "; EnableVoiceMute=1 mutes Dollman gameplay voice lines.\n"
    "; EnableSubtitleMute=1 mutes Dollman gameplay subtitles.\n"
    "; Runtime hotkeys:\n"
    ";   F8 = mark a fresh probe session window in DollmanMute.log\n"
    "; ScannerMode=0 keeps scanner audio unchanged.\n"
    "; ScannerMode=1 reduces scanner intensity.\n"
    "; ScannerMode=2 fully mutes scanner audio.\n"
    "\n"
    "[General]\n"
    "Enabled=1\n"
    "VerboseLog=0\n"
    "EnableVoiceMute=1\n"
    "EnableSubtitleMute=1\n"
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

static BOOL read_ini_bool_compat(
    const char *section,
    const char *key,
    const char *legacy_key,
    BOOL default_value)
{
    int value = GetPrivateProfileIntA(section, key, -1, g_ini_path);
    if (value == -1 && legacy_key != NULL) {
        value = GetPrivateProfileIntA(section, legacy_key, -1, g_ini_path);
    }
    if (value == -1) {
        value = default_value ? 1 : 0;
    }
    return value != 0;
}

static int read_ini_int_compat(
    const char *section,
    const char *key,
    const char *legacy_key,
    int default_value)
{
    int value = GetPrivateProfileIntA(section, key, -1, g_ini_path);
    if (value == -1 && legacy_key != NULL) {
        value = GetPrivateProfileIntA(section, legacy_key, -1, g_ini_path);
    }
    if (value == -1) {
        value = default_value;
    }
    return value;
}

static void load_config(void)
{
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    g_cfg.enabled = TRUE;
    g_cfg.verbose_log = FALSE;
    g_cfg.enable_dollman_radio_mute = TRUE;
    g_cfg.enable_throw_recall_subtitle_mute = FALSE;
    g_cfg.active_subtitle_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
    g_cfg.enable_sender_only_runtime_mode = TRUE;
    g_cfg.enable_subtitle_runtime_hooks = TRUE;
    g_cfg.enable_legacy_runtime_wrapper = FALSE;
    g_cfg.enable_subtitle_family_tracking = FALSE;
    g_cfg.enable_subtitle_producer_probe = FALSE;
    g_cfg.enable_builder_probe = FALSE;
    g_cfg.enable_selector_probe = FALSE;
    g_cfg.enable_deep_probe = FALSE;
    g_cfg.enable_talk_dispatcher_probe = FALSE;
    g_cfg.scanner_mode = SCANNER_MODE_OFF;

    ensure_default_ini();

    g_cfg.enabled = read_ini_bool_compat("General", "Enabled", NULL, g_cfg.enabled);
    g_cfg.verbose_log = read_ini_bool_compat("General", "VerboseLog", NULL, g_cfg.verbose_log);
    g_cfg.enable_dollman_radio_mute = read_ini_bool_compat(
        "General",
        "EnableVoiceMute",
        "EnableDollmanRadioMute",
        g_cfg.enable_dollman_radio_mute);
    g_cfg.enable_subtitle_runtime_hooks = read_ini_bool_compat(
        "General",
        "EnableSubtitleMute",
        "EnableSubtitleRuntimeHooks",
        g_cfg.enable_subtitle_runtime_hooks);
    {
        int scanner_mode_value = read_ini_int_compat(
            "General",
            "ScannerMode",
            NULL,
            (int)g_cfg.scanner_mode);
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

static const char *subtitle_strategy_name(uint32_t strategy)
{
    if (strategy < SUBTITLE_STRATEGY_COUNT) {
        return k_subtitle_strategy_meta[strategy].name;
    }
    return "unknown";
}

static BOOL is_vk_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static BOOL is_stf_probe_window_open(void)
{
    LONG64 until_ms = InterlockedCompareExchange64(&g_stf_probe_window_until_ms, 0, 0);

    if (until_ms == 0) {
        return FALSE;
    }
    return GetTickCount64() <= (ULONGLONG)until_ms;
}

static void reset_stf_probe_cache(void)
{
    if (!g_stf_probe_lock_inited) {
        return;
    }

    EnterCriticalSection(&g_stf_probe_lock);
    ZeroMemory(g_stf_probe_cache, sizeof(g_stf_probe_cache));
    g_stf_probe_cache_count = 0;
    LeaveCriticalSection(&g_stf_probe_lock);
}

static BOOL stf_probe_seen_or_mark(uint64_t key)
{
    int i;
    BOOL seen = FALSE;

    if (key == 0 || !g_stf_probe_lock_inited) {
        return FALSE;
    }

    EnterCriticalSection(&g_stf_probe_lock);
    for (i = 0; i < g_stf_probe_cache_count; ++i) {
        if (g_stf_probe_cache[i] == key) {
            seen = TRUE;
            break;
        }
    }
    if (!seen && g_stf_probe_cache_count < STF_PROBE_CACHE_MAX) {
        g_stf_probe_cache[g_stf_probe_cache_count++] = key;
    }
    LeaveCriticalSection(&g_stf_probe_lock);

    return seen;
}

static void reset_dollman_starttalk_sound_cache(void)
{
    ZeroMemory(g_dollman_starttalk_sounds, sizeof(g_dollman_starttalk_sounds));
    ZeroMemory(g_dollman_starttalk_instances, sizeof(g_dollman_starttalk_instances));
    InterlockedExchange(&g_dollman_starttalk_sound_cursor, 0);
    InterlockedExchange(&g_dollman_starttalk_instance_cursor, 0);
    InterlockedExchange(&g_starttalk_bridge_sample_hits, 0);
}

static void note_dollman_starttalk_sound(uintptr_t sound, uintptr_t line, uintptr_t voice, BOOL should_mute)
{
    LONG slot;

    if (sound == 0) {
        return;
    }

    slot = InterlockedIncrement(&g_dollman_starttalk_sound_cursor);
    slot = (slot - 1) % DOLLMAN_STARTTALK_SOUND_CACHE_MAX;
    g_dollman_starttalk_sounds[slot].sound = sound;
    g_dollman_starttalk_sounds[slot].line = line;
    g_dollman_starttalk_sounds[slot].voice = voice;
    g_dollman_starttalk_sounds[slot].should_mute = should_mute;
    g_dollman_starttalk_sounds[slot].seen_ms = GetTickCount64();
}

static BOOL is_recent_dollman_starttalk_sound(uintptr_t ptr, BOOL should_mute, ULONGLONG now_ms, ULONGLONG *delta_ms_out)
{
    int i;

    if (ptr == 0) {
        return FALSE;
    }

    for (i = 0; i < DOLLMAN_STARTTALK_SOUND_CACHE_MAX; ++i) {
        ULONGLONG seen_ms = g_dollman_starttalk_sounds[i].seen_ms;
        if (seen_ms == 0 ||
            g_dollman_starttalk_sounds[i].sound != ptr ||
            g_dollman_starttalk_sounds[i].should_mute != should_mute) {
            continue;
        }
        if (now_ms < seen_ms || (now_ms - seen_ms) > DOLLMAN_STARTTALK_CACHE_TTL_MS) {
            continue;
        }
        if (delta_ms_out != NULL) {
            *delta_ms_out = now_ms - seen_ms;
        }
        return TRUE;
    }

    return FALSE;
}

static void note_dollman_starttalk_instance(uintptr_t sound_instance, uintptr_t sound_resource, BOOL should_mute)
{
    LONG slot;

    if (sound_instance == 0) {
        return;
    }

    slot = InterlockedIncrement(&g_dollman_starttalk_instance_cursor);
    slot = (slot - 1) % DOLLMAN_STARTTALK_SOUND_CACHE_MAX;
    g_dollman_starttalk_instances[slot].sound_instance = sound_instance;
    g_dollman_starttalk_instances[slot].sound_resource = sound_resource;
    g_dollman_starttalk_instances[slot].should_mute = should_mute;
    g_dollman_starttalk_instances[slot].seen_ms = GetTickCount64();
}

static BOOL is_recent_dollman_starttalk_instance(uintptr_t ptr, BOOL should_mute, ULONGLONG now_ms, ULONGLONG *delta_ms_out)
{
    int i;

    if (ptr == 0) {
        return FALSE;
    }

    for (i = 0; i < DOLLMAN_STARTTALK_SOUND_CACHE_MAX; ++i) {
        ULONGLONG seen_ms = g_dollman_starttalk_instances[i].seen_ms;
        if (seen_ms == 0 ||
            g_dollman_starttalk_instances[i].sound_instance != ptr ||
            g_dollman_starttalk_instances[i].should_mute != should_mute) {
            continue;
        }
        if (now_ms < seen_ms || (now_ms - seen_ms) > DOLLMAN_STARTTALK_CACHE_TTL_MS) {
            continue;
        }
        if (delta_ms_out != NULL) {
            *delta_ms_out = now_ms - seen_ms;
        }
        return TRUE;
    }

    return FALSE;
}

static BOOL is_voice_queue_probe_caller(uintptr_t caller_rva)
{
    return caller_rva == k_rva_voice_queue_shared_helper_return ||
           caller_rva == k_rva_voice_queue_dispatcher_synth_return ||
           caller_rva == k_rva_voice_queue_dispatcher_forward_return;
}

static const char *voice_queue_probe_caller_name(uintptr_t caller_rva)
{
    switch (caller_rva) {
    case k_rva_voice_queue_shared_helper_return:
        return "shared-helper";
    case k_rva_voice_queue_dispatcher_synth_return:
        return "dispatcher-synth";
    case k_rva_voice_queue_dispatcher_forward_return:
        return "dispatcher-forward";
    default:
        return "other";
    }
}

static uintptr_t get_return_address_value(void)
{
#if defined(__clang__) || defined(__GNUC__)
    return (uintptr_t)__builtin_return_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_ReturnAddress();
#else
    return 0;
#endif
}

static void reset_strategy_stats(void)
{
    size_t i;

    for (i = 0; i < SUBTITLE_STRATEGY_COUNT; ++i) {
        InterlockedExchange(&g_strategy_stats[i].evaluated, 0);
        InterlockedExchange(&g_strategy_stats[i].would_mute, 0);
        InterlockedExchange(&g_strategy_stats[i].actual_mute, 0);
    }
}

static void reset_hotkey_runtime_state(void)
{
    ZeroMemory(g_hotkey_control_prev, sizeof(g_hotkey_control_prev));
}

static void reset_session_probe_state(void)
{
    InterlockedExchange(&g_session_counter, 0);
    InterlockedExchange64(&g_stf_probe_window_until_ms, 0);
    reset_stf_probe_cache();
}

static void reset_runtime_capture_counters(void)
{
    InterlockedExchange(&g_subtitle_runtime_hits, 0);
    InterlockedExchange(&g_subtitle_remove_hits, 0);
}

static void reset_log_capture_state(void)
{
    reset_runtime_capture_counters();
    reset_stf_probe_cache();
    InterlockedExchange64(&g_last_dowser_subtitle_ms, 0);
    g_last_dowser_key2 = 0;
    g_last_dowser_key3 = 0;
    g_last_dowser_p6 = 0;
    g_last_dowser_p7 = 0;
    InterlockedExchange64(&g_last_dollman_muted_subtitle_ms, 0);
    g_last_dollman_muted_speaker_tag = 0;
    g_last_dollman_muted_line_tag = 0;
    g_last_dollman_muted_caller_rva = 0;
}

static void seed_hotkey_state_from_config(void)
{
    EnterCriticalSection(&g_hotkey_lock);
    g_active_subtitle_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
    LeaveCriticalSection(&g_hotkey_lock);
}

static uint32_t get_active_subtitle_strategy(void)
{
    uint32_t active_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;

    EnterCriticalSection(&g_hotkey_lock);
    active_strategy = g_active_subtitle_strategy;
    LeaveCriticalSection(&g_hotkey_lock);

    return active_strategy;
}

static BOOL is_dowser_gameplay_subtitle(
    uintptr_t caller_rva,
    uint32_t speaker_tag,
    BOOL speaker_tag_valid,
    uint32_t line_tag,
    BOOL line_tag_valid)
{
    return caller_rva == k_dollman_gameplay_caller_rva &&
           speaker_tag_valid &&
           line_tag_valid &&
           speaker_tag == k_dowser_gameplay_speaker_tag &&
           line_tag == k_dowser_gameplay_line_tag;
}

static void note_dowser_gameplay_subtitle(
    uintptr_t caller_rva,
    const uint64_t *words,
    size_t word_count,
    uint32_t speaker_tag,
    uint32_t line_tag)
{
    ULONGLONG now_ms = GetTickCount64();

    if (words != NULL) {
        g_last_dowser_key2 = (word_count > 2) ? words[2] : 0;
        g_last_dowser_key3 = (word_count > 3) ? words[3] : 0;
        g_last_dowser_p6 = (uintptr_t)((word_count > 6) ? words[6] : 0);
        g_last_dowser_p7 = (uintptr_t)((word_count > 7) ? words[7] : 0);
    } else {
        g_last_dowser_key2 = 0;
        g_last_dowser_key3 = 0;
        g_last_dowser_p6 = 0;
        g_last_dowser_p7 = 0;
    }
    InterlockedExchange64(&g_last_dowser_subtitle_ms, (LONG64)now_ms);

    if (is_stf_probe_window_open()) {
        log_line(
            "DowserSubtitleSample caller_rva=0x%llx speaker_tag=0x%x line_tag=0x%x q2=0x%llx q3=0x%llx p6=0x%llx p7=0x%llx",
            (unsigned long long)caller_rva,
            (unsigned int)speaker_tag,
            (unsigned int)line_tag,
            (unsigned long long)g_last_dowser_key2,
            (unsigned long long)g_last_dowser_key3,
            (unsigned long long)g_last_dowser_p6,
            (unsigned long long)g_last_dowser_p7);
    }
}

static BOOL get_recent_dowser_subtitle_delta_ms(
    ULONGLONG now_ms,
    ULONGLONG *delta_ms_out)
{
    LONG64 last_ms = InterlockedCompareExchange64(&g_last_dowser_subtitle_ms, 0, 0);
    ULONGLONG delta_ms = 0;

    if (delta_ms_out != NULL) {
        *delta_ms_out = 0;
    }
    if (last_ms <= 0 || now_ms < (ULONGLONG)last_ms) {
        return FALSE;
    }

    delta_ms = now_ms - (ULONGLONG)last_ms;
    if (delta_ms_out != NULL) {
        *delta_ms_out = delta_ms;
    }
    return delta_ms <= 500ull;
}

static void note_dollman_muted_subtitle(
    uintptr_t caller_rva,
    uint32_t speaker_tag,
    uint32_t line_tag)
{
    g_last_dollman_muted_caller_rva = caller_rva;
    g_last_dollman_muted_speaker_tag = speaker_tag;
    g_last_dollman_muted_line_tag = line_tag;
    InterlockedExchange64(&g_last_dollman_muted_subtitle_ms, (LONG64)GetTickCount64());
}

static BOOL get_recent_dollman_muted_subtitle_delta_ms(
    ULONGLONG now_ms,
    ULONGLONG *delta_ms_out)
{
    LONG64 last_ms = InterlockedCompareExchange64(&g_last_dollman_muted_subtitle_ms, 0, 0);
    ULONGLONG delta_ms = 0;

    if (delta_ms_out != NULL) {
        *delta_ms_out = 0;
    }
    if (last_ms <= 0 || now_ms < (ULONGLONG)last_ms) {
        return FALSE;
    }

    delta_ms = now_ms - (ULONGLONG)last_ms;
    if (delta_ms_out != NULL) {
        *delta_ms_out = delta_ms;
    }

    return delta_ms <= 1500ull &&
           g_last_dollman_muted_caller_rva == k_dollman_gameplay_caller_rva &&
           g_last_dollman_muted_speaker_tag == k_dollman_gameplay_speaker_tag;
}

static BOOL is_selected_subtitle_family(uint32_t family)
{
    if (family == SUBTITLE_FAMILY_THROW_RECALL) {
        return g_cfg.enable_throw_recall_subtitle_mute;
    }
    return FALSE;
}

static uintptr_t tls_get_current_subtitle_family(void)
{
    if (g_tls_current_subtitle_family == TLS_OUT_OF_INDEXES) {
        return SUBTITLE_FAMILY_NONE;
    }
    return (uintptr_t)TlsGetValue(g_tls_current_subtitle_family);
}

static void tls_set_current_subtitle_family(uintptr_t family)
{
    if (g_tls_current_subtitle_family == TLS_OUT_OF_INDEXES) {
        return;
    }
    TlsSetValue(g_tls_current_subtitle_family, (LPVOID)family);
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
    uint64_t word1 = safe_read_u64(ptr + sizeof(uint64_t));
    return (uint32_t)(word1 >> 32);
}

static uint32_t classify_subtitle_family_from_identity_tag(uint32_t identity_tag)
{
    if (identity_tag == k_identity_tag_throw_recall) {
        return SUBTITLE_FAMILY_THROW_RECALL;
    }
    if (identity_tag == k_identity_tag_dialogue_a ||
        identity_tag == k_identity_tag_dialogue_b) {
        return SUBTITLE_FAMILY_DIALOGUE;
    }
    return SUBTITLE_FAMILY_NONE;
}

static uint32_t classify_subtitle_family(uintptr_t pre_p200_field72)
{
    return classify_subtitle_family_from_identity_tag(read_identity_hi32(pre_p200_field72));
}

static BOOL is_gameplay_dollman_pair(
    uintptr_t caller_rva,
    uint32_t speaker_tag,
    BOOL speaker_tag_valid)
{
    return speaker_tag_valid &&
           caller_rva == k_dollman_gameplay_caller_rva &&
           speaker_tag == k_dollman_gameplay_speaker_tag;
}

static BOOL should_mute_gameplay_throw_recall_preamble(const ShowStrategyContext *ctx)
{
    (void)ctx;
    return FALSE;
}

static BOOL subtitle_strategy_uses_family_tracking(uint32_t strategy)
{
    return strategy == SUBTITLE_STRATEGY_SELECTED_FAMILY ||
           strategy == SUBTITLE_STRATEGY_PAIR_OR_SELECTED_FAMILY;
}

static BOOL is_subtitle_runtime_mute_enabled(void)
{
    return g_cfg.enabled && g_cfg.enable_subtitle_runtime_hooks;
}

static BOOL is_sender_only_runtime_mode_enabled(void)
{
    return g_cfg.enable_subtitle_runtime_hooks &&
           g_cfg.enable_sender_only_runtime_mode;
}

static BOOL is_legacy_dollman_radio_mute_enabled(void)
{
    return g_cfg.enabled &&
           g_cfg.enable_dollman_radio_mute &&
           !is_sender_only_runtime_mode_enabled();
}

static BOOL is_sender_only_dollman_radio_mute_enabled(void)
{
    return g_cfg.enabled &&
           g_cfg.enable_dollman_radio_mute &&
           is_sender_only_runtime_mode_enabled();
}

static BOOL is_sender_only_random_chatter_event(
    AkUniqueID event_id,
    uint32_t external_source_count,
    uint64_t ext0)
{
    if (external_source_count != 1u) {
        return FALSE;
    }

    if (event_id == k_event_id_dollman_fall_chatter) {
        return ext0 == k_dollman_fall_chatter_ext0_sample;
    }

    return FALSE;
}

static BOOL should_block_sender_only_event_id(
    AkUniqueID event_id,
    uint32_t external_source_count,
    uint64_t ext0)
{
    if (!is_sender_only_dollman_radio_mute_enabled()) {
        return FALSE;
    }

    return event_id == k_event_id_dollman_equip ||
           event_id == k_event_id_dollman_throw ||
           event_id == k_event_id_dollman_recall ||
           is_sender_only_random_chatter_event(
               event_id,
               external_source_count,
               ext0);
}

static uint32_t read_subtitle_runtime_prepare_token(void)
{
    uintptr_t runtime_ctx = safe_deref_qword((uintptr_t)resolve_rva(k_rva_subtitle_runtime_context));

    if (runtime_ctx == 0 ||
        IsBadReadPtr((const void *)(runtime_ctx + 48936), sizeof(uint32_t))) {
        return 0;
    }
    return *(const uint32_t *)(runtime_ctx + 48936);
}

static void log_subtitle_identity_probe(
    const char *surface,
    uintptr_t caller_rva,
    const uint64_t *words,
    size_t word_count)
{
    uint32_t tag0 = 0;
    uint32_t tag1 = 0;
    uint32_t tag6 = 0;
    uint32_t tag7 = 0;
    char text0[96];
    char text1[96];
    char text6[96];
    char text7[96];
    BOOL ok0 = FALSE;
    BOOL ok1 = FALSE;
    BOOL ok6 = FALSE;
    BOOL ok7 = FALSE;

    if (words == NULL || word_count == 0) {
        return;
    }

    ZeroMemory(text0, sizeof(text0));
    ZeroMemory(text1, sizeof(text1));
    ZeroMemory(text6, sizeof(text6));
    ZeroMemory(text7, sizeof(text7));

    if (word_count > 0 && words[0] != 0) {
        ok0 = read_localized_text_resource((uintptr_t)words[0], &tag0, text0, sizeof(text0));
    }
    if (word_count > 1 && words[1] != 0) {
        ok1 = read_localized_text_resource((uintptr_t)words[1], &tag1, text1, sizeof(text1));
    }
    if (word_count > 6 && words[6] != 0) {
        ok6 = read_localized_text_resource((uintptr_t)words[6], &tag6, text6, sizeof(text6));
    }
    if (word_count > 7 && words[7] != 0) {
        ok7 = read_localized_text_resource((uintptr_t)words[7], &tag7, text7, sizeof(text7));
    }

    log_line(
        "SubtitleKey surface=%s caller_rva=0x%llx key0=0x%llx key1=0x%llx q2=0x%llx q3=0x%llx p6=0x%llx p7=0x%llx "
        "k0_ok=%d k0_tag=0x%x k0_text=\"%s\" k1_ok=%d k1_tag=0x%x k1_text=\"%s\" "
        "p6_ok=%d p6_tag=0x%x p6_text=\"%s\" p7_ok=%d p7_tag=0x%x p7_text=\"%s\"",
        surface != NULL ? surface : "?",
        (unsigned long long)caller_rva,
        (unsigned long long)((word_count > 0) ? words[0] : 0),
        (unsigned long long)((word_count > 1) ? words[1] : 0),
        (unsigned long long)((word_count > 2) ? words[2] : 0),
        (unsigned long long)((word_count > 3) ? words[3] : 0),
        (unsigned long long)((word_count > 6) ? words[6] : 0),
        (unsigned long long)((word_count > 7) ? words[7] : 0),
        ok0 ? 1 : 0,
        (unsigned int)tag0,
        text0,
        ok1 ? 1 : 0,
        (unsigned int)tag1,
        text1,
        ok6 ? 1 : 0,
        (unsigned int)tag6,
        text6,
        ok7 ? 1 : 0,
        (unsigned int)tag7,
        text7);

    if (!ok6 && word_count > 6 && words[6] != 0) {
        log_localized_text_resource_candidate("sender.p6", (uintptr_t)words[6]);
    }
    if (!ok7 && word_count > 7 && words[7] != 0) {
        log_localized_text_resource_candidate("sender.p7", (uintptr_t)words[7]);
    }
}

static void log_remove_subtitle_probe(
    uintptr_t caller_rva,
    const uint64_t *key_pair,
    char mode)
{
    uint64_t words[2] = {0};
    uint32_t tag0 = 0;
    uint32_t tag1 = 0;
    char text0[96];
    char text1[96];
    BOOL ok0 = FALSE;
    BOOL ok1 = FALSE;

    if (key_pair == NULL || IsBadReadPtr(key_pair, sizeof(words))) {
        log_line(
            "SubtitleRemove caller_rva=0x%llx unreadable=1 mode=%u",
            (unsigned long long)caller_rva,
            (unsigned int)(uint8_t)mode);
        return;
    }

    ZeroMemory(text0, sizeof(text0));
    ZeroMemory(text1, sizeof(text1));
    memcpy(words, key_pair, sizeof(words));

    if (words[0] != 0) {
        ok0 = read_localized_text_resource((uintptr_t)words[0], &tag0, text0, sizeof(text0));
    }
    if (words[1] != 0) {
        ok1 = read_localized_text_resource((uintptr_t)words[1], &tag1, text1, sizeof(text1));
    }

    log_line(
        "SubtitleRemove caller_rva=0x%llx key0=0x%llx key1=0x%llx mode=%u "
        "k0_ok=%d k0_tag=0x%x k0_text=\"%s\" k1_ok=%d k1_tag=0x%x k1_text=\"%s\"",
        (unsigned long long)caller_rva,
        (unsigned long long)words[0],
        (unsigned long long)words[1],
        (unsigned int)(uint8_t)mode,
        ok0 ? 1 : 0,
        (unsigned int)tag0,
        text0,
        ok1 ? 1 : 0,
        (unsigned int)tag1,
        text1);
}

static BOOL should_mute_show_ctx(const ShowStrategyContext *ctx, uint32_t strategy)
{
    if (ctx == NULL) {
        return FALSE;
    }

    switch (strategy) {
    case SUBTITLE_STRATEGY_OBSERVE:
        return FALSE;
    case SUBTITLE_STRATEGY_GAMEPLAY_PAIR:
        return is_gameplay_dollman_pair(ctx->caller_rva, ctx->speaker_tag, ctx->speaker_tag_valid) ||
               should_mute_gameplay_throw_recall_preamble(ctx);
    case SUBTITLE_STRATEGY_CALLER_ONLY:
        return ctx->caller_rva == k_dollman_gameplay_caller_rva;
    case SUBTITLE_STRATEGY_SPEAKER_ONLY:
        return ctx->speaker_tag_valid && ctx->speaker_tag == k_dollman_gameplay_speaker_tag;
    case SUBTITLE_STRATEGY_SELECTED_FAMILY:
        return is_selected_subtitle_family(ctx->current_family);
    case SUBTITLE_STRATEGY_PAIR_OR_SELECTED_FAMILY:
        return is_gameplay_dollman_pair(ctx->caller_rva, ctx->speaker_tag, ctx->speaker_tag_valid) ||
               is_selected_subtitle_family(ctx->current_family);
    default:
        return FALSE;
    }
}

static BOOL process_subtitle_payload(
    const char *surface,
    uintptr_t caller_rva,
    const uint64_t *payload)
{
    uintptr_t raw_current_family = tls_get_current_subtitle_family();
    uint32_t last_builder = tls_get_last_builder();
    uint32_t active_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
    uint64_t words[8] = {0};
    BOOL payload_readable = FALSE;
    BOOL speaker_tag_valid = FALSE;
    BOOL line_tag_valid = FALSE;
    uint32_t speaker_tag = 0;
    uint32_t line_tag = 0;
    uint32_t derived_family = SUBTITLE_FAMILY_NONE;
    ShowStrategyContext strategy_ctx;
    BOOL strategy_results[SUBTITLE_STRATEGY_COUNT];
    BOOL actual_mute = FALSE;
    LONG hit_index = 0;
    uint32_t i;

    active_strategy = get_active_subtitle_strategy();

    if (payload != NULL && !IsBadReadPtr(payload, sizeof(words))) {
        memcpy(words, payload, sizeof(words));
        payload_readable = TRUE;
        if (words[6] != 0) {
            line_tag_valid = read_localized_text_resource(
                (uintptr_t)words[6],
                &line_tag,
                NULL,
                0);
        }
        if (line_tag_valid) {
            derived_family = classify_subtitle_family_from_identity_tag(line_tag);
        }
        if (words[7] != 0) {
            speaker_tag_valid = read_localized_text_resource(
                (uintptr_t)words[7],
                &speaker_tag,
                NULL,
                0);
        }
    }

    strategy_ctx.caller_rva = caller_rva;
    strategy_ctx.current_family =
        (derived_family != SUBTITLE_FAMILY_NONE)
            ? derived_family
            : (uint32_t)raw_current_family;
    strategy_ctx.speaker_tag = speaker_tag;
    strategy_ctx.speaker_tag_valid = speaker_tag_valid;
    strategy_ctx.last_builder = last_builder;

    if (is_dowser_gameplay_subtitle(
            caller_rva,
            speaker_tag,
            speaker_tag_valid,
            line_tag,
            line_tag_valid)) {
        note_dowser_gameplay_subtitle(
            caller_rva,
            words,
            sizeof(words) / sizeof(words[0]),
            speaker_tag,
            line_tag);
    }

    for (i = 0; i < SUBTITLE_STRATEGY_COUNT; ++i) {
        BOOL would_mute = should_mute_show_ctx(&strategy_ctx, i);
        strategy_results[i] = would_mute;
        InterlockedIncrement(&g_strategy_stats[i].evaluated);
        if (would_mute) {
            InterlockedIncrement(&g_strategy_stats[i].would_mute);
        }
    }

    hit_index = InterlockedIncrement(&g_subtitle_runtime_hits);
    if ((g_cfg.verbose_log || g_cfg.enable_deep_probe) && hit_index <= 24) {
        log_line(
            "SubtitleHit surface=%s caller_rva=0x%llx speaker_ok=%d speaker_tag=0x%x line_ok=%d line_tag=0x%x family=%s builder=%s",
            surface != NULL ? surface : "?",
            (unsigned long long)caller_rva,
            speaker_tag_valid ? 1 : 0,
            (unsigned int)speaker_tag,
            line_tag_valid ? 1 : 0,
            (unsigned int)line_tag,
            subtitle_family_name(strategy_ctx.current_family),
            (last_builder < BUILDER_ID_COUNT) ? k_builder_names[last_builder] : "none");
        log_subtitle_identity_probe(surface, caller_rva, words, sizeof(words) / sizeof(words[0]));
    }

    if (is_subtitle_runtime_mute_enabled() &&
        active_strategy < SUBTITLE_STRATEGY_COUNT &&
        strategy_results[active_strategy]) {
        actual_mute = TRUE;
        InterlockedIncrement(&g_strategy_stats[active_strategy].actual_mute);
        if (speaker_tag_valid && line_tag_valid) {
            note_dollman_muted_subtitle(caller_rva, speaker_tag, line_tag);
        }
        log_line(
            "Muted subtitle surface=%s strategy=%s caller_rva=0x%llx speaker_ok=%d speaker_tag=0x%x line_ok=%d line_tag=0x%x family=%s builder=%s",
            surface != NULL ? surface : "?",
            subtitle_strategy_name(active_strategy),
            (unsigned long long)caller_rva,
            speaker_tag_valid ? 1 : 0,
            (unsigned int)speaker_tag,
            line_tag_valid ? 1 : 0,
            (unsigned int)line_tag,
            subtitle_family_name(strategy_ctx.current_family),
            (last_builder < BUILDER_ID_COUNT) ? k_builder_names[last_builder] : "none");
    }

    if (is_stf_probe_window_open() &&
        (g_cfg.enable_subtitle_producer_probe ||
         g_cfg.enable_builder_probe ||
         g_cfg.enable_selector_probe)) {
        uint64_t p6_data[8] = {0};
        uint64_t p7_data[8] = {0};
        uintptr_t vtbl_rva = 0;
        uintptr_t p6_vtbl_rva = 0;
        uintptr_t p7_vtbl_rva = 0;
        if (payload_readable) {
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
            "[show] surface=%s tid=%lu caller_rva=0x%llx r=%d vtbl_rva=0x%llx p=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx] p6v=0x%llx p6=[0x%llx,0x%llx,0x%llx,0x%llx] p7v=0x%llx p7=[0x%llx,0x%llx,0x%llx,0x%llx]",
            surface != NULL ? surface : "?",
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)caller_rva,
            payload_readable ? 1 : 0,
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

        log_line(
            "[strategy] surface=%s tid=%lu active=%s builder=%s family=%s speaker_ok=%d speaker_tag=0x%x caller_rva=0x%llx "
            "obs=%d pair=%d caller=%d speaker=%d selected=%d hybrid=%d actual=%d",
            surface != NULL ? surface : "?",
            (unsigned long)GetCurrentThreadId(),
            subtitle_strategy_name(active_strategy),
            (last_builder < BUILDER_ID_COUNT) ? k_builder_names[last_builder] : "none",
            subtitle_family_name(strategy_ctx.current_family),
            speaker_tag_valid ? 1 : 0,
            (unsigned int)speaker_tag,
            (unsigned long long)caller_rva,
            strategy_results[SUBTITLE_STRATEGY_OBSERVE] ? 1 : 0,
            strategy_results[SUBTITLE_STRATEGY_GAMEPLAY_PAIR] ? 1 : 0,
            strategy_results[SUBTITLE_STRATEGY_CALLER_ONLY] ? 1 : 0,
            strategy_results[SUBTITLE_STRATEGY_SPEAKER_ONLY] ? 1 : 0,
            strategy_results[SUBTITLE_STRATEGY_SELECTED_FAMILY] ? 1 : 0,
            strategy_results[SUBTITLE_STRATEGY_PAIR_OR_SELECTED_FAMILY] ? 1 : 0,
            actual_mute ? 1 : 0);
    }

    log_verbose(
        "[show] surface=%s strategy=%s family=%s speaker_ok=%d speaker=0x%x caller=0x%llx builder=%s actual=%d",
        surface != NULL ? surface : "?",
        subtitle_strategy_name(active_strategy),
        subtitle_family_name(strategy_ctx.current_family),
        speaker_tag_valid ? 1 : 0,
        (unsigned int)speaker_tag,
        (unsigned long long)caller_rva,
        (last_builder < BUILDER_ID_COUNT) ? k_builder_names[last_builder] : "none",
        actual_mute ? 1 : 0);

    return actual_mute;
}

static void update_hotkey_mute_state(void)
{
    BOOL key_control_down[HOTKEY_CONTROL_COUNT];
    uint32_t i;

    for (i = 0; i < HOTKEY_CONTROL_COUNT; ++i) {
        key_control_down[i] = is_vk_down(k_hotkey_control_vks[i]);
    }

    if (key_control_down[HOTKEY_CONTROL_SESSION_MARK] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_SESSION_MARK]) {
        LONG counter = InterlockedIncrement(&g_session_counter);
        ULONGLONG until_ms = GetTickCount64() + 5000ull;
        InterlockedExchange64(&g_stf_probe_window_until_ms, (LONG64)until_ms);
        reset_log_capture_state();
        log_line("=== session boundary F8 count=%ld ===", (long)counter);
    }

    for (i = 0; i < HOTKEY_CONTROL_COUNT; ++i) {
        g_hotkey_control_prev[i] = key_control_down[i];
    }
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

    if (!is_legacy_dollman_radio_mute_enabled()) {
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
    if (status == MH_ERROR_ALREADY_CREATED) {
        if (original == NULL || *original == NULL) {
            log_line(
                "Failed to create hook %s: already created but original trampoline is unavailable",
                label != NULL ? label : "(unknown)");
            return FALSE;
        }
    } else if (status != MH_OK) {
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

static BOOL is_known_dollman_audio_event(AkUniqueID event_id)
{
    return event_id == k_event_id_dollman_equip ||
           event_id == k_event_id_dollman_throw ||
           event_id == k_event_id_dollman_recall ||
           event_id == k_event_id_dollman_fall_chatter;
}

static BOOL is_dollman_voice_resource(uintptr_t voice, uint32_t *speaker_tag_out, char *speaker_text, size_t speaker_text_size)
{
    static const unsigned int k_voice_speaker_offsets[] = {0x28u, 0x30u, 0x38u, 0x40u, 0x48u};
    size_t i;

    if (speaker_tag_out != NULL) {
        *speaker_tag_out = 0;
    }
    if (speaker_text != NULL && speaker_text_size > 0) {
        speaker_text[0] = '\0';
    }

    for (i = 0; i < sizeof(k_voice_speaker_offsets) / sizeof(k_voice_speaker_offsets[0]); ++i) {
        uintptr_t text_ref = safe_read_ptr(voice + k_voice_speaker_offsets[i]);
        uint32_t tag = 0;
        char text[96];
        BOOL ok;

        ZeroMemory(text, sizeof(text));
        ok = read_localized_text_resource(text_ref, &tag, text, sizeof(text));
        if (!ok) {
            continue;
        }

        if (tag == k_dollman_gameplay_speaker_tag || strcmp(text, "Dollman") == 0) {
            if (speaker_tag_out != NULL) {
                *speaker_tag_out = tag;
            }
            if (speaker_text != NULL && speaker_text_size > 0) {
                snprintf(speaker_text, speaker_text_size, "%s", text);
            }
            return TRUE;
        }
    }

    return FALSE;
}

static uintptr_t __fastcall hook_start_talk_get_or_create_sound_wrapper(uintptr_t starttalk, uintptr_t out_wrapper)
{
    uintptr_t caller = get_return_address_value();
    uintptr_t caller_rva = (g_image_base != 0 && caller > g_image_base)
        ? (caller - g_image_base)
        : 0;
    uintptr_t slot = safe_read_ptr(starttalk + 0xC8);
    uintptr_t line = safe_read_ptr(slot + 0x0);
    uintptr_t sound = safe_read_ptr(line + 0x38);
    uintptr_t voice_fallback = safe_read_ptr(line + 0x50);
    uintptr_t voice_preferred = safe_read_ptr(line + 0x58);
    uint32_t starttalk_flags = safe_read_u32(starttalk + 0x68);
    uint32_t dollman_speaker_tag = 0;
    char dollman_speaker_text[96];
    uint32_t fallback_speaker_tag = 0;
    uint32_t preferred_speaker_tag = 0;
    char fallback_speaker_text[96];
    char preferred_speaker_text[96];
    BOOL fallback_is_dollman;
    BOOL preferred_is_dollman;
    BOOL interesting;
    uintptr_t selected_voice = 0;
    LONG sample_hit = InterlockedIncrement(&g_starttalk_bridge_sample_hits);

    ZeroMemory(dollman_speaker_text, sizeof(dollman_speaker_text));
    ZeroMemory(fallback_speaker_text, sizeof(fallback_speaker_text));
    ZeroMemory(preferred_speaker_text, sizeof(preferred_speaker_text));

    fallback_is_dollman = is_dollman_voice_resource(
        voice_fallback,
        &fallback_speaker_tag,
        fallback_speaker_text,
        sizeof(fallback_speaker_text));
    preferred_is_dollman = is_dollman_voice_resource(
        voice_preferred,
        &preferred_speaker_tag,
        preferred_speaker_text,
        sizeof(preferred_speaker_text));
    interesting = fallback_is_dollman || preferred_is_dollman;

    if (preferred_is_dollman) {
        selected_voice = voice_preferred;
        dollman_speaker_tag = preferred_speaker_tag;
        snprintf(dollman_speaker_text, sizeof(dollman_speaker_text), "%s", preferred_speaker_text);
    } else if (fallback_is_dollman) {
        selected_voice = voice_fallback;
        dollman_speaker_tag = fallback_speaker_tag;
        snprintf(dollman_speaker_text, sizeof(dollman_speaker_text), "%s", fallback_speaker_text);
    } else if (voice_preferred != 0) {
        selected_voice = voice_preferred;
    } else {
        selected_voice = voice_fallback;
    }

    if (interesting) {
        uint64_t dedupe_key =
            ((uint64_t)starttalk >> 4) ^
            ((uint64_t)line << 5) ^
            ((uint64_t)sound >> 7) ^
            ((uint64_t)selected_voice << 11) ^
            0x5354575241505045ull;

        if (fallback_is_dollman || preferred_is_dollman) {
            BOOL should_mute = starttalk_flags == 0u;
            uintptr_t body = safe_read_ptr(line + 0x48);
            uint32_t body_tag = 0;
            char body_text[160];
            BOOL body_ok;

            ZeroMemory(body_text, sizeof(body_text));
            body_ok = read_localized_text_resource(body, &body_tag, body_text, sizeof(body_text));
            note_dollman_starttalk_sound(sound, line, selected_voice, should_mute);
            log_line(
                "DollmanStartTalkCandidate mute=%d flags=0x%x caller_rva=0x%llx line=0x%llx sound=0x%llx voice=0x%llx speaker_tag=0x%x speaker_text=\"%s\" body_ok=%d body_tag=0x%x body_text=\"%s\"",
                should_mute ? 1 : 0,
                (unsigned int)starttalk_flags,
                (unsigned long long)caller_rva,
                (unsigned long long)line,
                (unsigned long long)sound,
                (unsigned long long)selected_voice,
                (unsigned int)dollman_speaker_tag,
                dollman_speaker_text,
                body_ok ? 1 : 0,
                (unsigned int)body_tag,
                body_text);
        }

        if ((g_cfg.verbose_log || g_cfg.enable_deep_probe) && !stf_probe_seen_or_mark(dedupe_key)) {
            uintptr_t body = safe_read_ptr(line + 0x48);
            uint32_t entry_count = safe_read_u32(starttalk + 0x78);
            uintptr_t entry_data = safe_read_ptr(starttalk + 0x80);
            uintptr_t inline_slot = starttalk + 0x98;
            uintptr_t slot_delta = (slot >= inline_slot && slot < starttalk + 0xC0)
                ? (slot - inline_slot)
                : 0;
            uint32_t body_tag = 0;
            char body_text[160];
            BOOL body_ok;
            uintptr_t selected_voice_vtbl = safe_read_ptr(selected_voice + 0x0);
            uintptr_t selected_voice_refs[5];
            uint32_t selected_voice_tags[5];
            char selected_voice_texts[5][96];
            BOOL selected_voice_oks[5];
            static const unsigned int k_voice_offsets[] = {0x28u, 0x30u, 0x38u, 0x40u, 0x48u};
            size_t i;

            ZeroMemory(body_text, sizeof(body_text));
            ZeroMemory(selected_voice_refs, sizeof(selected_voice_refs));
            ZeroMemory(selected_voice_tags, sizeof(selected_voice_tags));
            ZeroMemory(selected_voice_texts, sizeof(selected_voice_texts));
            ZeroMemory(selected_voice_oks, sizeof(selected_voice_oks));
            body_ok = read_localized_text_resource(body, &body_tag, body_text, sizeof(body_text));
            if (selected_voice != 0) {
                for (i = 0; i < sizeof(k_voice_offsets) / sizeof(k_voice_offsets[0]); ++i) {
                    selected_voice_refs[i] = safe_read_ptr(selected_voice + k_voice_offsets[i]);
                    selected_voice_oks[i] = read_localized_text_resource(
                        selected_voice_refs[i],
                        &selected_voice_tags[i],
                        selected_voice_texts[i],
                        sizeof(selected_voice_texts[i]));
                }
            }

            log_line(
                "[starttalk-sound-wrapper] sample=%ld caller_rva=0x%llx starttalk=0x%llx out=0x%llx "
                "flags=0x%x entry_count=%u entry_data=0x%llx inline_slot=0x%llx slot_delta=0x%llx "
                "slot=0x%llx line=0x%llx sound=0x%llx body=0x%llx body_ok=%d body_tag=0x%x body_text=\"%s\" "
                "voice_fallback=0x%llx voice_preferred=0x%llx selected_voice=0x%llx selected_voice_vtbl=0x%llx "
                "fallback_dollman=%d preferred_dollman=%d speaker_tag=0x%x speaker_text=\"%s\" "
                "v28=0x%llx ok28=%d tag28=0x%x text28=\"%s\" "
                "v30=0x%llx ok30=%d tag30=0x%x text30=\"%s\" "
                "v38=0x%llx ok38=%d tag38=0x%x text38=\"%s\" "
                "v40=0x%llx ok40=%d tag40=0x%x text40=\"%s\" "
                "v48=0x%llx ok48=%d tag48=0x%x text48=\"%s\"",
                (long)sample_hit,
                (unsigned long long)caller_rva,
                (unsigned long long)starttalk,
                (unsigned long long)out_wrapper,
                (unsigned int)starttalk_flags,
                (unsigned int)entry_count,
                (unsigned long long)entry_data,
                (unsigned long long)inline_slot,
                (unsigned long long)slot_delta,
                (unsigned long long)slot,
                (unsigned long long)line,
                (unsigned long long)sound,
                (unsigned long long)body,
                body_ok ? 1 : 0,
                (unsigned int)body_tag,
                body_text,
                (unsigned long long)voice_fallback,
                (unsigned long long)voice_preferred,
                (unsigned long long)selected_voice,
                (unsigned long long)selected_voice_vtbl,
                fallback_is_dollman ? 1 : 0,
                preferred_is_dollman ? 1 : 0,
                (unsigned int)dollman_speaker_tag,
                dollman_speaker_text,
                (unsigned long long)selected_voice_refs[0],
                selected_voice_oks[0] ? 1 : 0,
                (unsigned int)selected_voice_tags[0],
                selected_voice_texts[0],
                (unsigned long long)selected_voice_refs[1],
                selected_voice_oks[1] ? 1 : 0,
                (unsigned int)selected_voice_tags[1],
                selected_voice_texts[1],
                (unsigned long long)selected_voice_refs[2],
                selected_voice_oks[2] ? 1 : 0,
                (unsigned int)selected_voice_tags[2],
                selected_voice_texts[2],
                (unsigned long long)selected_voice_refs[3],
                selected_voice_oks[3] ? 1 : 0,
                (unsigned int)selected_voice_tags[3],
                selected_voice_texts[3],
                (unsigned long long)selected_voice_refs[4],
                selected_voice_oks[4] ? 1 : 0,
                (unsigned int)selected_voice_tags[4],
                selected_voice_texts[4]);
        }
    }

    if (g_real_start_talk_get_or_create_sound_wrapper == NULL) {
        return out_wrapper;
    }
    return g_real_start_talk_get_or_create_sound_wrapper(starttalk, out_wrapper);
}

static int __fastcall hook_sound_instance_submit(
    AkUniqueID event_id,
    uintptr_t sound_instance,
    uint8_t a3,
    uint8_t a4,
    uintptr_t s0,
    uintptr_t s1,
    uintptr_t s2,
    uintptr_t s3,
    uintptr_t s4,
    uintptr_t s5,
    uintptr_t s6,
    uintptr_t s7,
    uintptr_t s8,
    uintptr_t s9)
{
    ULONGLONG now_ms = GetTickCount64();
    ULONGLONG starttalk_sound_delta_ms = 0;
    uintptr_t p178 = safe_read_ptr(sound_instance + 0x178);
    uintptr_t p178_0 = safe_read_ptr(p178 + 0x0);
    uintptr_t owner = safe_read_ptr(p178_0 + 0x20);
    BOOL starttalk_sound_match =
        is_recent_dollman_starttalk_sound(owner, TRUE, now_ms, &starttalk_sound_delta_ms) ||
        is_recent_dollman_starttalk_sound(p178_0, TRUE, now_ms, &starttalk_sound_delta_ms);
    BOOL starttalk_sound_bypass =
        is_recent_dollman_starttalk_sound(owner, FALSE, now_ms, NULL) ||
        is_recent_dollman_starttalk_sound(p178_0, FALSE, now_ms, NULL);

    if (starttalk_sound_match) {
        note_dollman_starttalk_instance(sound_instance, owner, TRUE);
        log_line(
            "DollmanStartTalkBound mute=1 eventId=%u soundInstance=0x%llx owner=0x%llx p178_0=0x%llx deltaMs=%llu",
            (unsigned int)event_id,
            (unsigned long long)sound_instance,
            (unsigned long long)owner,
            (unsigned long long)p178_0,
            (unsigned long long)starttalk_sound_delta_ms);
    } else if (starttalk_sound_bypass) {
        note_dollman_starttalk_instance(sound_instance, owner, FALSE);
        log_line(
            "DollmanStartTalkBound mute=0 eventId=%u soundInstance=0x%llx owner=0x%llx p178_0=0x%llx",
            (unsigned int)event_id,
            (unsigned long long)sound_instance,
            (unsigned long long)owner,
            (unsigned long long)p178_0);
    }

    if (g_cfg.verbose_log || g_cfg.enable_deep_probe) {
        uint32_t external_source_count = (uint32_t)s8;
        uintptr_t ext_ptr = s9;
        uint64_t ext0 = safe_read_u64(ext_ptr + 0x0);
        uint64_t ext1 = safe_read_u64(ext_ptr + 0x8);
        uint64_t ext2 = safe_read_u64(ext_ptr + 0x10);
        uint64_t ext3 = safe_read_u64(ext_ptr + 0x18);
        ULONGLONG dollman_delta_ms = 0;
        BOOL recent_dollman_subtitle =
            external_source_count == 1u &&
            get_recent_dollman_muted_subtitle_delta_ms(now_ms, &dollman_delta_ms);
        BOOL interesting =
            is_known_dollman_audio_event(event_id) ||
            event_id == k_event_id_dowser_gameplay_chatter ||
            recent_dollman_subtitle ||
            starttalk_sound_match;

        if (interesting) {
        uintptr_t inst_vtbl = safe_read_ptr(sound_instance + 0x0);
        uint64_t p178_8 = safe_read_u64(p178 + 0x8);
        uintptr_t owner_vtbl = safe_read_ptr(owner + 0x0);
        uint32_t owner_tag = safe_read_u32(owner + 0x0C);
        uintptr_t owner_line = owner + 0x3E0;
        uintptr_t owner_line_vtbl = safe_read_ptr(owner_line + 0x0);
        uint32_t owner_line_tag = 0;
        char owner_line_text[160];
        BOOL owner_line_ok;
        uint32_t flags60 = safe_read_u32(sound_instance + 0x60);
        uint32_t flags184 = safe_read_u32(sound_instance + 0x184);
        uint32_t slot_count = safe_read_u32(sound_instance + 0x250);
        uintptr_t slot_ptr = safe_read_ptr(sound_instance + 0x258);
        uint32_t owner_bc = safe_read_u32(owner + 0xBC);
        uint64_t dedupe_key =
            ((uint64_t)event_id) ^
            ((uint64_t)sound_instance >> 3) ^
            ((uint64_t)owner << 7) ^
            ((uint64_t)ext0 << 11) ^
            0x534E44494E535455ull;

        ZeroMemory(owner_line_text, sizeof(owner_line_text));
        owner_line_ok = read_localized_text_resource(
            owner_line,
            &owner_line_tag,
            owner_line_text,
            sizeof(owner_line_text));

        if (!stf_probe_seen_or_mark(dedupe_key)) {
            log_line(
                "[sound-instance-submit] eventId=%u sound=0x%llx inst_vtbl=0x%llx a3=0x%x a4=0x%x "
                "extCount=%u ext_ptr=0x%llx ext0=0x%llx ext1=0x%llx ext2=0x%llx ext3=0x%llx flags60=0x%x flags184=0x%x "
                "p178=0x%llx p178_0=0x%llx p178_8=0x%llx owner=0x%llx owner_vtbl=0x%llx owner_tag=0x%x owner_bc=0x%x "
                "owner_line=0x%llx owner_line_vtbl=0x%llx owner_line_ok=%d owner_line_tag=0x%x owner_line_text=\"%s\" "
                "recent_dollman=%d deltaMs=%llu starttalk_sound_match=%d starttalkDeltaMs=%llu "
                "slot_count=%u slot_ptr=0x%llx s0=0x%llx s1=0x%llx s2=0x%llx s3=0x%llx",
                (unsigned int)event_id,
                (unsigned long long)sound_instance,
                (unsigned long long)inst_vtbl,
                (unsigned int)a3,
                (unsigned int)a4,
                (unsigned int)external_source_count,
                (unsigned long long)ext_ptr,
                (unsigned long long)ext0,
                (unsigned long long)ext1,
                (unsigned long long)ext2,
                (unsigned long long)ext3,
                (unsigned int)flags60,
                (unsigned int)flags184,
                (unsigned long long)p178,
                (unsigned long long)p178_0,
                (unsigned long long)p178_8,
                (unsigned long long)owner,
                (unsigned long long)owner_vtbl,
                (unsigned int)owner_tag,
                (unsigned int)owner_bc,
                (unsigned long long)owner_line,
                (unsigned long long)owner_line_vtbl,
                owner_line_ok ? 1 : 0,
                (unsigned int)owner_line_tag,
                owner_line_text,
                recent_dollman_subtitle ? 1 : 0,
                (unsigned long long)dollman_delta_ms,
                starttalk_sound_match ? 1 : 0,
                (unsigned long long)starttalk_sound_delta_ms,
                (unsigned int)slot_count,
                (unsigned long long)slot_ptr,
                (unsigned long long)s0,
                (unsigned long long)s1,
                (unsigned long long)s2,
                (unsigned long long)s3);
        }
        }
    }

    return g_real_sound_instance_submit(
        event_id,
        sound_instance,
        a3,
        a4,
        s0,
        s1,
        s2,
        s3,
        s4,
        s5,
        s6,
        s7,
        s8,
        s9);
}

static BOOL patch_pointer_slot(
    uintptr_t slot_rva,
    void *replacement,
    void *expected_original,
    void **original_out,
    void ***slot_out,
    const char *label)
{
    void **slot = (void **)resolve_rva(slot_rva);
    void *original = NULL;
    DWORD old_protect = 0;

    if (slot == NULL || replacement == NULL) {
        log_line("Skip pointer patch %s: slot or replacement missing", label != NULL ? label : "(unknown)");
        return FALSE;
    }

    if (IsBadReadPtr(slot, sizeof(void *))) {
        log_line("Skip pointer patch %s: slot unreadable at %p", label != NULL ? label : "(unknown)", slot);
        return FALSE;
    }

    original = *slot;
    if (expected_original != NULL && original != expected_original) {
        log_line(
            "Skip pointer patch %s: live original=%p expected=%p slot=%p",
            label != NULL ? label : "(unknown)",
            original,
            expected_original,
            slot);
        return FALSE;
    }
    if (original_out != NULL) {
        *original_out = original;
    }

    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        log_line("Failed to reprotect pointer slot %s: %lu", label != NULL ? label : "(unknown)", (unsigned long)GetLastError());
        return FALSE;
    }

    *slot = replacement;
    VirtualProtect(slot, sizeof(void *), old_protect, &old_protect);
    FlushProcessWriteBuffers();

    if (slot_out != NULL) {
        *slot_out = slot;
    }

    log_line(
        "Patched %s slot=%p original=%p replacement=%p",
        label != NULL ? label : "(unknown)",
        slot,
        original,
        replacement);
    return TRUE;
}

static void restore_pointer_slot(void **slot, void *original, const char *label)
{
    DWORD old_protect = 0;

    if (slot == NULL || original == NULL) {
        return;
    }

    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        log_line("Failed to reprotect pointer slot for restore %s: %lu", label != NULL ? label : "(unknown)", (unsigned long)GetLastError());
        return;
    }

    *slot = original;
    VirtualProtect(slot, sizeof(void *), old_protect, &old_protect);
    FlushProcessWriteBuffers();
    log_line("Restored %s slot=%p original=%p", label != NULL ? label : "(unknown)", slot, original);
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
    BOOL probe_enabled =
        is_stf_probe_window_open() &&
        (g_cfg.enable_selector_probe ||
         g_cfg.enable_deep_probe ||
         is_sender_only_dollman_radio_mute_enabled());
    BOOL blocked_legacy = g_cfg.enabled && should_block_event_id(event_id);
    BOOL blocked_sender_only = FALSE;
    BOOL blocked_starttalk_speaker = FALSE;
    BOOL bypass_starttalk_speaker = FALSE;
    BOOL blocked = FALSE;
    BOOL dowser_event_match = FALSE;
    BOOL dowser_ext_match = FALSE;
    BOOL dowser_recent = FALSE;
    const char *block_mode = "none";
    uintptr_t ext_ptr = (uintptr_t)external_sources;
    uint64_t ext0 = safe_read_u64(ext_ptr + 0x0);
    uint64_t dedupe_key = 0;
    ULONGLONG dowser_delta_ms = 0;
    ULONGLONG starttalk_delta_ms = 0;
    ULONGLONG now_ms = GetTickCount64();

    bypass_starttalk_speaker =
        g_cfg.enabled &&
        is_sender_only_dollman_radio_mute_enabled() &&
        external_source_count == 1u &&
        is_recent_dollman_starttalk_instance(
            (uintptr_t)game_object_id,
            FALSE,
            now_ms,
            NULL);
    blocked_starttalk_speaker =
        g_cfg.enabled &&
        is_sender_only_dollman_radio_mute_enabled() &&
        external_source_count == 1u &&
        !bypass_starttalk_speaker &&
        is_recent_dollman_starttalk_instance(
            (uintptr_t)game_object_id,
            TRUE,
            now_ms,
            &starttalk_delta_ms);
    blocked_sender_only =
        g_cfg.enabled &&
        !blocked_starttalk_speaker &&
        !bypass_starttalk_speaker &&
        should_block_sender_only_event_id(
            event_id,
            external_source_count,
            ext0);
    blocked = blocked_legacy || blocked_sender_only || blocked_starttalk_speaker;
    if (blocked_sender_only) {
        block_mode = "sender-only-narrow";
    } else if (blocked_starttalk_speaker) {
        block_mode = "sender-only-starttalk-speaker";
    } else if (blocked_legacy) {
        block_mode = "legacy";
    }

    if (probe_enabled) {
        uintptr_t caller_ra = get_return_address_value();
        uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
            ? (caller_ra - g_image_base)
            : 0;
        uint64_t ext1 = safe_read_u64(ext_ptr + 0x8);
        uint64_t ext2 = safe_read_u64(ext_ptr + 0x10);
        uint64_t ext3 = safe_read_u64(ext_ptr + 0x18);

        dedupe_key = ((uint64_t)event_id) ^
                     (((uint64_t)caller_rva) << 32) ^
                     (((uint64_t)(uint32_t)external_source_count) << 19) ^
                     (((uint64_t)game_object_id) >> 7) ^
                     (((uint64_t)ext0) << 3) ^
                    (((uint64_t)ext1) >> 11) ^
                    0x504F53544556454Eull;
        if (!stf_probe_seen_or_mark(dedupe_key)) {
            log_line(
                "[postevent] caller_rva=0x%llx eventId=%u gameObject=0x%llx externalSources=%u ext_ptr=0x%llx ext0=0x%llx ext1=0x%llx blocked=%d playingIdIn=%u",
                (unsigned long long)caller_rva,
                (unsigned int)event_id,
                (unsigned long long)game_object_id,
                (unsigned int)external_source_count,
                (unsigned long long)ext_ptr,
                (unsigned long long)ext0,
                (unsigned long long)ext1,
                blocked ? 1 : 0,
                (unsigned int)playing_id);
        }

        dowser_event_match = event_id == k_event_id_dowser_gameplay_chatter;
        dowser_ext_match = ext0 == k_dowser_ext0_sample;
        dowser_recent =
            external_source_count == 1 &&
            get_recent_dowser_subtitle_delta_ms(now_ms, &dowser_delta_ms);
        if (dowser_event_match || dowser_ext_match || dowser_recent) {
            log_line(
                "DowserPostEventCandidate caller_rva=0x%llx eventId=%u extCount=%u ext0=0x%llx ext1=0x%llx ext2=0x%llx ext3=0x%llx "
                "eventMatch=%d ext0Match=%d recentSubtitle=%d deltaMs=%llu key2=0x%llx key3=0x%llx p6=0x%llx p7=0x%llx blocked=%d",
                (unsigned long long)caller_rva,
                (unsigned int)event_id,
                (unsigned int)external_source_count,
                (unsigned long long)ext0,
                (unsigned long long)ext1,
                (unsigned long long)ext2,
                (unsigned long long)ext3,
                dowser_event_match ? 1 : 0,
                dowser_ext_match ? 1 : 0,
                dowser_recent ? 1 : 0,
                (unsigned long long)dowser_delta_ms,
                (unsigned long long)g_last_dowser_key2,
                (unsigned long long)g_last_dowser_key3,
                (unsigned long long)g_last_dowser_p6,
                (unsigned long long)g_last_dowser_p7,
                blocked ? 1 : 0);
        }
        if (blocked_starttalk_speaker) {
            log_line(
                "StartTalkSpeakerPostEvent caller_rva=0x%llx eventId=%u extCount=%u ext0=0x%llx ext1=0x%llx deltaMs=%llu gameObject=0x%llx",
                (unsigned long long)caller_rva,
                (unsigned int)event_id,
                (unsigned int)external_source_count,
                (unsigned long long)ext0,
                (unsigned long long)ext1,
                (unsigned long long)starttalk_delta_ms,
                (unsigned long long)game_object_id);
        }
        if (bypass_starttalk_speaker) {
            log_line(
                "BypassStartTalkSpeakerPostEvent caller_rva=0x%llx eventId=%u extCount=%u ext0=0x%llx ext1=0x%llx gameObject=0x%llx",
                (unsigned long long)caller_rva,
                (unsigned int)event_id,
                (unsigned int)external_source_count,
                (unsigned long long)ext0,
                (unsigned long long)ext1,
                (unsigned long long)game_object_id);
        }
    }

    if (blocked) {
        log_line(
            "Blocked PostEventID mode=%s eventId=%u gameObject=0x%llx externalSources=%u",
            block_mode,
            (unsigned int)event_id,
            (unsigned long long)game_object_id,
            (unsigned int)external_source_count);
    } else if (g_cfg.verbose_log) {
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

static uintptr_t __fastcall hook_show_subtitle(uintptr_t view, const uint64_t *payload)
{
    uintptr_t caller_ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                               ? (caller_ra - g_image_base)
                               : 0;
    if (process_subtitle_payload("sender", caller_rva, payload)) {
        log_verbose(
            "[mute] surface=sender caller=0x%llx",
            (unsigned long long)caller_rva);
        return 0;
    }

    return g_real_show_subtitle(view, payload);
}

static uintptr_t __fastcall hook_remove_subtitle(uintptr_t view, const uint64_t *key_pair, char mode)
{
    uintptr_t caller_ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                               ? (caller_ra - g_image_base)
                               : 0;
    LONG hit_index = InterlockedIncrement(&g_subtitle_remove_hits);

    if ((g_cfg.verbose_log || g_cfg.enable_deep_probe) && hit_index <= 24) {
        log_remove_subtitle_probe(caller_rva, key_pair, mode);
    }

    return g_real_remove_subtitle(view, key_pair, mode);
}

static uintptr_t safe_deref_qword(uintptr_t addr)
{
    return (uintptr_t)safe_read_u64(addr);
}

static uintptr_t safe_read_ptr(uintptr_t addr)
{
    if (addr == 0) {
        return 0;
    }
    if (IsBadReadPtr((const void *)addr, sizeof(uintptr_t))) {
        return 0;
    }
    return *(const uintptr_t *)addr;
}

static uint64_t safe_read_u64(uintptr_t addr)
{
    if (addr == 0) {
        return 0;
    }
    if (IsBadReadPtr((const void *)addr, sizeof(uint64_t))) {
        return 0;
    }
    return *(const uint64_t *)addr;
}

static uint32_t safe_read_u32(uintptr_t addr)
{
    if (addr == 0) {
        return 0;
    }
    if (IsBadReadPtr((const void *)addr, sizeof(uint32_t))) {
        return 0;
    }
    return *(const uint32_t *)addr;
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

static void log_localized_text_resource_candidate(
    const char *label,
    uintptr_t ptr)
{
    uintptr_t vtbl = 0;
    uintptr_t vtbl_rva = 0;
    uint64_t word1 = 0;
    uintptr_t text_ptr = 0;
    uint64_t text_len = 0;
    char preview[96];
    size_t copy_len = 0;

    if (ptr == 0) {
        log_line(
            "SubtitleCandidate label=%s ptr=0x0 unreadable=1",
            label != NULL ? label : "?");
        return;
    }

    if (IsBadReadPtr((const void *)ptr, 0x30)) {
        log_line(
            "SubtitleCandidate label=%s ptr=0x%llx unreadable=1",
            label != NULL ? label : "?",
            (unsigned long long)ptr);
        return;
    }

    ZeroMemory(preview, sizeof(preview));
    vtbl = *(const uintptr_t *)(ptr + 0x0);
    if (g_image_base != 0 && vtbl > g_image_base) {
        vtbl_rva = vtbl - g_image_base;
    }
    word1 = *(const uint64_t *)(ptr + 0x8);
    text_ptr = *(const uintptr_t *)(ptr + 0x20);
    text_len = *(const uint64_t *)(ptr + 0x28);

    if (text_ptr != 0 &&
        text_len != 0 &&
        text_len < sizeof(preview) &&
        !IsBadReadPtr((const void *)text_ptr, (size_t)text_len)) {
        copy_len = (size_t)text_len;
        memcpy(preview, (const void *)text_ptr, copy_len);
        preview[copy_len] = '\0';
    }

    log_line(
        "SubtitleCandidate label=%s ptr=0x%llx vtbl=0x%llx vtbl_rva=0x%llx looks_ltr=%d "
        "word1=0x%llx hi32=0x%x text_ptr=0x%llx text_len=0x%llx preview=\"%s\"",
        label != NULL ? label : "?",
        (unsigned long long)ptr,
        (unsigned long long)vtbl,
        (unsigned long long)vtbl_rva,
        (vtbl_rva == k_rva_localized_text_resource_vtbl) ? 1 : 0,
        (unsigned long long)word1,
        (unsigned int)(word1 >> 32),
        (unsigned long long)text_ptr,
        (unsigned long long)text_len,
        preview);
}

__declspec(dllexport) int core_init(const ProxyContext *ctx)
{
    MH_STATUS status;
    unsigned int hook_count = 0;
    BOOL need_show_subtitle_hook = FALSE;
    BOOL show_subtitle_hook_installed = FALSE;
    BOOL subtitle_runtime_surface_enabled = FALSE;
    BOOL sender_only_runtime_mode = FALSE;
    BOOL effective_dollman_radio_mute = FALSE;
    BOOL sender_only_dollman_voice_mute = FALSE;

    ZeroMemory(&g_proxy_ctx, sizeof(g_proxy_ctx));
    if (ctx != NULL) {
        g_proxy_ctx = *ctx;
    }

    InterlockedExchange(&g_core_shutting_down, 0);

    InitializeCriticalSection(&g_log_lock);
    g_log_lock_inited = TRUE;
    InitializeCriticalSection(&g_stf_probe_lock);
    g_stf_probe_lock_inited = TRUE;
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
    g_tls_current_subtitle_family = TlsAlloc();
    if (g_tls_current_subtitle_family == TLS_OUT_OF_INDEXES) {
        log_line("TlsAlloc(current_family) failed: %lu", (unsigned long)GetLastError());
    }
    g_tls_last_builder = TlsAlloc();
    if (g_tls_last_builder == TLS_OUT_OF_INDEXES) {
        log_line("TlsAlloc(last_builder) failed: %lu", (unsigned long)GetLastError());
    }
    reset_strategy_stats();
    reset_hotkey_runtime_state();
    reset_session_probe_state();
    reset_dollman_starttalk_sound_cache();
    reset_log_capture_state();
    seed_hotkey_state_from_config();
    g_active_subtitle_strategy = get_active_subtitle_strategy();
    subtitle_runtime_surface_enabled = g_cfg.enable_subtitle_runtime_hooks;
    sender_only_runtime_mode = is_sender_only_runtime_mode_enabled();
    sender_only_dollman_voice_mute = is_sender_only_dollman_radio_mute_enabled();
    effective_dollman_radio_mute = is_legacy_dollman_radio_mute_enabled();

    log_line("DollmanMute build: %s", k_build_tag);
    log_line("DollmanMute image_base=0x%llx image_size=0x%llx", (unsigned long long)g_image_base, (unsigned long long)g_image_size);
    log_line(
        "DollmanMute init start: enabled=%d verbose=%d voiceMute=%d subtitleMute=%d scannerMode=%u",
        g_cfg.enabled,
        g_cfg.verbose_log,
        g_cfg.enable_dollman_radio_mute,
        subtitle_runtime_surface_enabled,
        (unsigned int)g_cfg.scanner_mode);

    need_show_subtitle_hook = subtitle_runtime_surface_enabled;

    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("MH_Initialize failed: %d", (int)status);
        return 0;
    }

    if (sender_only_dollman_voice_mute) {
        if (install_rva_hook(
                k_rva_start_talk_get_or_create_sound_wrapper,
                hook_start_talk_get_or_create_sound_wrapper,
                (void **)&g_real_start_talk_get_or_create_sound_wrapper,
                "StartTalkFunction.GetOrCreateSoundWrapper.sub_140387FB0")) {
            ++hook_count;
            log_line("StartTalk sound-wrapper bridge probe active via sub_140387FB0 (pass-through)");
        }

        if (install_rva_hook(
                k_rva_sound_instance_submit,
                hook_sound_instance_submit,
                (void **)&g_real_sound_instance_submit,
                "SoundInstanceSubmit.sub_14026B8410")) {
            ++hook_count;
            log_line("Sound instance submit probe active via sub_14026B8410 (pass-through)");
        }
    } else {
        log_line("Sound instance submit probe disabled");
    }

    if (effective_dollman_radio_mute ||
        g_cfg.scanner_mode != SCANNER_MODE_OFF ||
        sender_only_dollman_voice_mute) {
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

    if (need_show_subtitle_hook) {
        if (install_rva_hook(
                k_rva_show_subtitle,
                hook_show_subtitle,
                (void **)&g_real_show_subtitle,
                "GameViewGame.ShowSubtitleSender.sub_140780BF0")) {
            show_subtitle_hook_installed = TRUE;
            ++hook_count;
        } else {
            log_line("Subtitle sender hook unavailable on this build");
        }

        if (install_rva_hook(
                k_rva_remove_subtitle,
                hook_remove_subtitle,
                (void **)&g_real_remove_subtitle,
                "GameViewGame.RemoveSubtitleSender.sub_140780CF0")) {
            ++hook_count;
        } else {
            log_line("Subtitle remove sender hook unavailable on this build");
        }
    } else {
        log_line("ShowSubtitle hook disabled by config");
    }

    if (subtitle_runtime_surface_enabled && !show_subtitle_hook_installed) {
        log_line("Subtitle runtime surface requested but ShowSubtitle hook is unavailable on this build");
    }

    g_hotkey_thread_handle = CreateThread(NULL, 0, hotkey_thread_proc, NULL, 0, NULL);
    if (g_hotkey_thread_handle != NULL) {
        log_line("Hotkeys active: F8=session mark");
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

    restore_pointer_slot(
        g_show_subtitle_vtable_slot,
        g_show_subtitle_vtable_original,
        "GameViewGame.vtbl[ShowSubtitle]");
    g_show_subtitle_vtable_slot = NULL;
    g_show_subtitle_vtable_original = NULL;

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    {
        size_t si;
        for (si = 0; si < SUBTITLE_STRATEGY_COUNT; ++si) {
            log_line(
                "Strategy summary: name=%s evaluated=%ld wouldMute=%ld actualMute=%ld",
                subtitle_strategy_name((uint32_t)si),
                (long)g_strategy_stats[si].evaluated,
                (long)g_strategy_stats[si].would_mute,
                (long)g_strategy_stats[si].actual_mute);
        }
    }

    g_real_post_event_id = NULL;
    g_real_sound_instance_submit = NULL;
    g_real_start_talk_get_or_create_sound_wrapper = NULL;
    g_real_show_subtitle = NULL;
    g_real_remove_subtitle = NULL;

    g_image_base = 0;
    reset_session_probe_state();
    reset_hotkey_runtime_state();
    reset_dollman_starttalk_sound_cache();

    log_line("core_shutdown complete");

    if (g_hotkey_lock_inited) {
        DeleteCriticalSection(&g_hotkey_lock);
        g_hotkey_lock_inited = FALSE;
    }
    if (g_stf_probe_lock_inited) {
        DeleteCriticalSection(&g_stf_probe_lock);
        g_stf_probe_lock_inited = FALSE;
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

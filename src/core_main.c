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
typedef uintptr_t(__fastcall *DollmanRadioPlayVoiceByControllerDelayFn)(
    uintptr_t instance,
    int controller_index);
typedef uintptr_t(__fastcall *VoiceSharedHelperFn)(
    uintptr_t manager_obj,
    uintptr_t voice_source,
    uintptr_t payload_source,
    int mode,
    unsigned int *sentence_key);
typedef char(__fastcall *VoiceQueueSubmitFn)(
    uintptr_t *queue_obj,
    unsigned int *request,
    char a3,
    uintptr_t a4,
    uintptr_t a5,
    unsigned char *a6);
typedef void(__fastcall *DollmanVoiceDelayClosureFn)(void *closure_state);
typedef uintptr_t(__fastcall *SubtitleRuntimeWrapperFn)(uintptr_t view, uintptr_t arg2);
typedef uintptr_t(__fastcall *ShowSubtitleFn)(uintptr_t view, const uint64_t *payload);
typedef uintptr_t(__fastcall *RemoveSubtitleFn)(uintptr_t view, const uint64_t *key_pair, char mode);
typedef uintptr_t(__fastcall *SubtitleProducerFn)(uintptr_t this_obj);
typedef uintptr_t(__fastcall *StartTalkInitFn)(uintptr_t this_obj);
typedef void(__fastcall *SubtitleRuntimePrepareFn)(
    uintptr_t view,
    uintptr_t arg2,
    uint32_t token);
typedef double(__fastcall *SubtitleRuntimeStageFn)(
    uintptr_t view,
    uintptr_t state,
    uintptr_t queue_a,
    uintptr_t queue_b,
    uintptr_t queue_c,
    uintptr_t queue_d);
typedef uintptr_t(__fastcall *SubtitleRuntimePayloadGetterFn)(
    uintptr_t state,
    void *scratch,
    void *meta);
typedef void(__fastcall *SubtitleRuntimeRenderFn)(uintptr_t view);
typedef uintptr_t(__fastcall *GameplaySinkFn)(
    uintptr_t rcx,
    uintptr_t rdx,
    uintptr_t r8,
    uintptr_t r9,
    uintptr_t a5,
    uintptr_t a6,
    uintptr_t a7,
    uintptr_t a8,
    uintptr_t a9);

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
static VoiceSharedHelperFn g_real_voice_shared_helper = NULL;
static VoiceQueueSubmitFn g_real_voice_queue_submit = NULL;
static DollmanVoiceDelayClosureFn g_real_dollman_voice_delay_closure = NULL;
static SubtitleRuntimeWrapperFn g_real_subtitle_runtime_wrapper = NULL;
static ShowSubtitleFn g_real_show_subtitle = NULL;
static RemoveSubtitleFn g_real_remove_subtitle = NULL;
static SubtitleProducerFn g_real_subtitle_producer = NULL;
static StartTalkInitFn g_real_start_talk_init = NULL;
static GameplaySinkFn g_real_gameplay_sink = NULL;
static void **g_show_subtitle_vtable_slot = NULL;
static void *g_show_subtitle_vtable_original = NULL;

static const char *k_build_tag = "research-v3.27f-postevent-narrow-block";

#define PRODUCER_IDENTITY_CACHE_MAX 4096
static uintptr_t g_image_base = 0;
static uintptr_t g_image_size = 0;
static const uintptr_t k_rva_localized_text_resource_vtbl = 0x03448D38u;

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
static volatile LONG64 g_stf_probe_window_until_ms = 0;
static CRITICAL_SECTION g_hotkey_lock;
static BOOL g_hotkey_lock_inited = FALSE;
static BOOL g_hotkey_throw_recall_mute = FALSE;
static BOOL g_hotkey_dialogue_mute = FALSE;
static DWORD g_tls_current_subtitle_family = TLS_OUT_OF_INDEXES;
static volatile LONG g_subtitle_runtime_hits = 0;
static volatile LONG g_subtitle_remove_hits = 0;

static const char *k_export_post_event_id =
    "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z";

/* Legacy broad audio hook names are kept for source continuity. On the current
 * build, 0x00C73BF0 lands in a ThroughDollmanInstance teardown path, not the
 * live delay wrapper. The Dollman-only runtime voice closure is 0x00C73EE0.
 * The old 0x00DAA410 "dispatcher" probe was a manager tick/update; the real
 * shared voice submit helper is 0x00DAC7B0. */
static const uintptr_t k_rva_dollman_radio_play_voice_by_controller_delay = 0x00C73BF0u;
static const uintptr_t k_rva_dollman_voice_delay_closure = 0x00C73EE0u;
static const uintptr_t k_rva_voice_shared_helper = 0x00DAC7B0u;
static const uintptr_t k_rva_voice_shared_helper_player_return = 0x00C73AEEu;
static const uintptr_t k_rva_voice_shared_helper_dollman_return = 0x00C73F6Du;
static const uintptr_t k_rva_voice_queue_submit = 0x00DAC910u;
static const uintptr_t k_rva_voice_queue_shared_helper_return = 0x00DAC891u;
static const uintptr_t k_rva_voice_queue_dispatcher_synth_return = 0x00DAAB64u;
static const uintptr_t k_rva_voice_queue_dispatcher_forward_return = 0x00DAC17Au;
static const uintptr_t k_rva_subtitle_runtime_wrapper = 0x00780690u;
static const uintptr_t k_rva_show_subtitle = 0x00780740u;
static const uintptr_t k_rva_remove_subtitle = 0x00780840u;
static const uintptr_t k_rva_subtitle_render = 0x007808A0u;
static const uintptr_t k_rva_subtitle_prepare = 0x0025A940u;
static const uintptr_t k_rva_subtitle_runtime_context = 0x062308B8u;
static const uintptr_t k_rva_game_view_game_show_subtitle_slot = 0x03188DE0u;
static const uintptr_t k_rva_subtitle_producer = 0x003872C0u;
static const uintptr_t k_rva_start_talk_init = 0x00387670u;
static const uintptr_t k_rva_selector_dispatch = 0x00DAF8A0u;
static const uintptr_t k_rva_talk_dispatcher = 0x00385720u;
static const uintptr_t k_rva_gameplay_sink = 0u;

/* Current build gameplay Dollman mute: observed (speaker tag, ShowSubtitle
 * caller RVA) pair for the chatter path. Older 0x38598B is stale on this
 * build; live sender now lands at 0x385C1B. */
static const uint32_t k_dollman_gameplay_speaker_tag = 0x12b6fu;
static const uintptr_t k_dollman_gameplay_caller_rva = 0x385c1bu;

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
    HOTKEY_CONTROL_THROW_RECALL_ON = 0u,
    HOTKEY_CONTROL_THROW_RECALL_OFF = 1u,
    HOTKEY_CONTROL_DIALOGUE_ON = 2u,
    HOTKEY_CONTROL_DIALOGUE_OFF = 3u,
    HOTKEY_CONTROL_CLEAR_ALL = 4u,
    HOTKEY_CONTROL_SESSION_MARK = 5u,
    HOTKEY_CONTROL_CLEAR_LOG = 6u,
    HOTKEY_CONTROL_COUNT = 7u
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
    { "pair", "mute only when caller 0x385C1B and speaker tag 0x12B6F both match", VK_F2 },
    { "callerOnly", "mute everything from caller 0x385C1B", VK_F3 },
    { "speakerOnly", "mute everything with speaker tag 0x12B6F", VK_F4 },
    { "selectedFamily", "mute only the subtitle families currently enabled by J/K/N/M", VK_F5 },
    { "pairOrSelectedFamily", "mute when the gameplay pair matches or the selected family matches", VK_F6 }
};
static const int k_hotkey_control_vks[HOTKEY_CONTROL_COUNT] = {
    'J', 'K', 'N', 'M', VK_F12, VK_F8, VK_F9
};
static const AkUniqueID k_event_id_dollman_throw = 2820786646u;
static const AkUniqueID k_event_id_dollman_recall = 2978848044u;

/* --- Builder probe (v3.14): log-only hooks on gameplay subtitle builders --- */
enum {
    BUILDER_ID_NONE = 0u,
    BUILDER_ID_A    = 1u, /* current build: unresolved, quarantined */
    BUILDER_ID_B    = 2u, /* sub_140350380 — MsgStartTalk handler */
    BUILDER_ID_C    = 3u, /* sub_140350790 — MsgDSStartTalk handler */
    BUILDER_ID_U1   = 4u, /* sub_140B5BC60 — unclassified 0x388950 caller */
    BUILDER_ID_U2   = 5u, /* sub_140B5CC20 — unclassified 0x388950 caller */
    BUILDER_ID_COUNT = 6u
};

static const char *k_builder_names[BUILDER_ID_COUNT] = {
    "none", "A", "B", "C", "U1", "U2"
};

static const uintptr_t k_rva_builder_a  = 0u;
static const uintptr_t k_rva_builder_b  = 0x00350380u;
static const uintptr_t k_rva_builder_c  = 0x00350790u;
static const uintptr_t k_rva_builder_u1 = 0x00B5BC60u;
static const uintptr_t k_rva_builder_u2 = 0x00B5CC20u;

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
static BOOL g_hotkey_strategy_prev[SUBTITLE_STRATEGY_COUNT] = {FALSE};
static BOOL g_hotkey_control_prev[HOTKEY_CONTROL_COUNT] = {FALSE};
static volatile LONG g_session_counter = 0;

static const AkUniqueID k_blocked_event_ids[] = {
    2995625663u, /* equip */
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
static const char *subtitle_strategy_desc(uint32_t strategy);
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
static void reset_builder_hit_counts(void);
static void reset_strategy_stats(void);
static void reset_hotkey_runtime_state(void);
static void reset_session_probe_state(void);
static void reset_runtime_capture_counters(void);
static void reset_identity_probe_cache(void);
static void reset_log_capture_state(void);
static void log_hotkey_mute_state(const char *key_name);
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
static BOOL should_block_sender_only_event_id(AkUniqueID event_id);
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

static const char *k_default_ini =
    "; DollmanMute runtime config.\n"
    "; VerboseLog=1 enables per-event mute logging for debugging.\n"
    "; EnableDollmanRadioMute=1 enables the current audio mute path.\n"
    ";   In sender-only mode it uses the Dollman-only voice closure path.\n"
    ";   Outside sender-only mode it falls back to the older broad legacy path.\n"
    ";   (hat/glasses reactions etc) together with their subtitles.\n"
    "; EnableThrowRecallSubtitleMute=1 sets startup default for throw/recall mute.\n"
    "; ActiveSubtitleStrategy selects which subtitle strategy actually mutes.\n"
    ";   0=observe, 1=pair, 2=callerOnly, 3=speakerOnly,\n"
    ";   4=selectedFamily, 5=pairOrSelectedFamily.\n"
    ";   observe=never mute, pair=must match caller+speaker,\n"
    ";   callerOnly=only caller match, speakerOnly=only speaker match,\n"
    ";   selectedFamily=respect J/K/N/M family toggles only,\n"
    ";   pairOrSelectedFamily=either pair match or selected family match.\n"
    "; EnableSenderOnlyRuntimeMode=1 keeps active mute on the current\n"
    ";   ShowSubtitle sender surface only. It suppresses the legacy audio mute\n"
    ";   hooks and skips the older runtime wrapper to minimize blast radius.\n"
    "; EnableSubtitleRuntimeHooks=1 arms ShowSubtitle-side runtime muting.\n"
    ";   Current research default keeps this ON.\n"
    "; EnableLegacyRuntimeWrapper=1 re-enables the older pre-sender runtime wrapper.\n"
    ";   It is OFF by default because sender-side muting is the stable surface.\n"
    "; EnableSubtitleFamilyTracking=1 keeps the older producer-side family probe\n"
    ";   available for research. Runtime family muting can classify directly from\n"
    ";   ShowSubtitle payload line identity tags on this build.\n"
    "; Current default profile keeps SelectorDispatch ON, but leaves\n"
    "; TalkDispatcher OFF because it currently causes runtime crashes.\n"
    "; EnableDeepProbe=1 explicitly arms StartTalk producer/init + Dollman voice\n"
    ";   correlation during F8 windows. Selector probe no longer piggybacks it.\n"
    "; Builder hooks remain OFF by default.\n"
    "; EnableBuilderProbe=1 installs log-only hooks on gameplay subtitle builders\n"
    ";   A=quarantined on current build, B=sub_140350380, C=sub_140350790,\n"
    ";   U1=sub_140B5BC60, U2=sub_140B5CC20.\n"
    ";   and emits windowed [show]/[builder]/[350c70] log lines.\n"
    ";   Used for mapping dollman behaviours to builder paths.\n"
    "; EnableSubtitleProducerProbe=1 additionally enables the newer\n"
    ";   StartTalk/init/shared-sink deep research hooks.\n"
    "; EnableTalkDispatcherProbe=1 is currently unsafe on this build.\n"
    "; Runtime hotkeys:\n"
    ";   J = throw/recall mute ON\n"
    ";   K = throw/recall mute OFF\n"
    ";   N = dollman dialogue mute ON\n"
    ";   M = dollman dialogue mute OFF\n"
    ";   F12 = clear all runtime subtitle mutes\n"
    ";   F1..F6 = switch active subtitle strategy\n"
    ";   F8  = mark session boundary + open 5s deep-probe window\n"
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
    "EnableDollmanRadioMute=1\n"
    "EnableThrowRecallSubtitleMute=0\n"
    "ActiveSubtitleStrategy=1\n"
    "EnableSenderOnlyRuntimeMode=1\n"
    "EnableSubtitleRuntimeHooks=1\n"
    "EnableLegacyRuntimeWrapper=0\n"
    "EnableSubtitleFamilyTracking=0\n"
    "EnableSubtitleProducerProbe=0\n"
    "EnableBuilderProbe=0\n"
    "EnableSelectorProbe=1\n"
    "EnableDeepProbe=0\n"
    "EnableTalkDispatcherProbe=0\n"
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
    g_cfg.enable_dollman_radio_mute = TRUE;
    g_cfg.enable_throw_recall_subtitle_mute = FALSE;
    g_cfg.active_subtitle_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
    g_cfg.enable_sender_only_runtime_mode = TRUE;
    g_cfg.enable_subtitle_runtime_hooks = TRUE;
    g_cfg.enable_legacy_runtime_wrapper = FALSE;
    g_cfg.enable_subtitle_family_tracking = FALSE;
    g_cfg.enable_subtitle_producer_probe = FALSE;
    g_cfg.enable_builder_probe = FALSE;
    g_cfg.enable_selector_probe = TRUE;
    g_cfg.enable_deep_probe = FALSE;
    g_cfg.enable_talk_dispatcher_probe = FALSE;
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
    g_cfg.active_subtitle_strategy = (uint32_t)GetPrivateProfileIntA(
        "General",
        "ActiveSubtitleStrategy",
        (int)g_cfg.active_subtitle_strategy,
        g_ini_path);
    g_cfg.enable_sender_only_runtime_mode = GetPrivateProfileIntA(
        "General",
        "EnableSenderOnlyRuntimeMode",
        g_cfg.enable_sender_only_runtime_mode,
        g_ini_path) != 0;
    g_cfg.enable_subtitle_runtime_hooks = GetPrivateProfileIntA(
        "General",
        "EnableSubtitleRuntimeHooks",
        g_cfg.enable_subtitle_runtime_hooks,
        g_ini_path) != 0;
    g_cfg.enable_legacy_runtime_wrapper = GetPrivateProfileIntA(
        "General",
        "EnableLegacyRuntimeWrapper",
        g_cfg.enable_legacy_runtime_wrapper,
        g_ini_path) != 0;
    g_cfg.enable_subtitle_family_tracking = GetPrivateProfileIntA(
        "General",
        "EnableSubtitleFamilyTracking",
        g_cfg.enable_subtitle_family_tracking,
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
    g_cfg.enable_deep_probe = GetPrivateProfileIntA(
        "General",
        "EnableDeepProbe",
        g_cfg.enable_deep_probe,
        g_ini_path) != 0;
    g_cfg.enable_talk_dispatcher_probe = GetPrivateProfileIntA(
        "General",
        "EnableTalkDispatcherProbe",
        g_cfg.enable_talk_dispatcher_probe,
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
    if (g_cfg.active_subtitle_strategy >= SUBTITLE_STRATEGY_COUNT) {
        g_cfg.active_subtitle_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
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

static void reset_builder_hit_counts(void)
{
    size_t i;

    for (i = 0; i < BUILDER_ID_COUNT; ++i) {
        InterlockedExchange(&g_builder_hit_counts[i], 0);
    }
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
    ZeroMemory(g_hotkey_strategy_prev, sizeof(g_hotkey_strategy_prev));
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

static void reset_identity_probe_cache(void)
{
    if (!g_identity_lock_inited) {
        return;
    }

    EnterCriticalSection(&g_identity_lock);
    g_identity_cache_count = 0;
    g_identity_cache_full_warned = FALSE;
    LeaveCriticalSection(&g_identity_lock);
}

static void reset_log_capture_state(void)
{
    reset_runtime_capture_counters();
    reset_stf_probe_cache();
    reset_identity_probe_cache();
}

static void log_hotkey_mute_state(const char *key_name)
{
    log_line(
        "HotkeyMute throwRecall=%d dialogue=%d key='%s'",
        g_hotkey_throw_recall_mute ? 1 : 0,
        g_hotkey_dialogue_mute ? 1 : 0,
        key_name != NULL ? key_name : "?");
}

static void seed_hotkey_state_from_config(void)
{
    EnterCriticalSection(&g_hotkey_lock);
    g_hotkey_throw_recall_mute = g_cfg.enable_throw_recall_subtitle_mute;
    g_hotkey_dialogue_mute = FALSE;
    if (g_cfg.active_subtitle_strategy < SUBTITLE_STRATEGY_COUNT) {
        g_active_subtitle_strategy = g_cfg.active_subtitle_strategy;
    } else {
        g_active_subtitle_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
    }
    LeaveCriticalSection(&g_hotkey_lock);
}

static void get_hotkey_mute_state(
    BOOL *throw_recall_mute,
    BOOL *dialogue_mute,
    uint32_t *active_strategy)
{
    EnterCriticalSection(&g_hotkey_lock);
    if (throw_recall_mute != NULL) {
        *throw_recall_mute = g_hotkey_throw_recall_mute;
    }
    if (dialogue_mute != NULL) {
        *dialogue_mute = g_hotkey_dialogue_mute;
    }
    if (active_strategy != NULL) {
        *active_strategy = g_active_subtitle_strategy;
    }
    LeaveCriticalSection(&g_hotkey_lock);
}

static BOOL is_selected_subtitle_family(uint32_t family)
{
    BOOL throw_recall_mute = FALSE;
    BOOL dialogue_mute = FALSE;

    get_hotkey_mute_state(&throw_recall_mute, &dialogue_mute, NULL);

    if (family == SUBTITLE_FAMILY_THROW_RECALL) {
        return throw_recall_mute;
    }
    if (family == SUBTITLE_FAMILY_DIALOGUE) {
        return dialogue_mute;
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

static BOOL should_block_sender_only_event_id(AkUniqueID event_id)
{
    if (!is_sender_only_dollman_radio_mute_enabled()) {
        return FALSE;
    }

    return event_id == k_event_id_dollman_throw ||
           event_id == k_event_id_dollman_recall;
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
        return is_gameplay_dollman_pair(ctx->caller_rva, ctx->speaker_tag, ctx->speaker_tag_valid);
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

    get_hotkey_mute_state(NULL, NULL, &active_strategy);

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

    for (i = 0; i < SUBTITLE_STRATEGY_COUNT; ++i) {
        BOOL would_mute = should_mute_show_ctx(&strategy_ctx, i);
        strategy_results[i] = would_mute;
        InterlockedIncrement(&g_strategy_stats[i].evaluated);
        if (would_mute) {
            InterlockedIncrement(&g_strategy_stats[i].would_mute);
        }
    }

    hit_index = InterlockedIncrement(&g_subtitle_runtime_hits);
    if (hit_index <= 24) {
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
    BOOL key_strategy_down[SUBTITLE_STRATEGY_COUNT];
    uint32_t i;

    for (i = 0; i < HOTKEY_CONTROL_COUNT; ++i) {
        key_control_down[i] = is_vk_down(k_hotkey_control_vks[i]);
    }
    for (i = 0; i < SUBTITLE_STRATEGY_COUNT; ++i) {
        key_strategy_down[i] = is_vk_down(k_subtitle_strategy_meta[i].vk);
    }

    if (key_control_down[HOTKEY_CONTROL_SESSION_MARK] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_SESSION_MARK]) {
        BOOL throw_recall_mute = FALSE;
        BOOL dialogue_mute = FALSE;
        uint32_t active_strategy = SUBTITLE_STRATEGY_GAMEPLAY_PAIR;
        LONG counter = InterlockedIncrement(&g_session_counter);
        ULONGLONG until_ms = GetTickCount64() + 5000ull;
        InterlockedExchange64(&g_stf_probe_window_until_ms, (LONG64)until_ms);
        reset_stf_probe_cache();
        reset_runtime_capture_counters();
        get_hotkey_mute_state(&throw_recall_mute, &dialogue_mute, &active_strategy);
        log_line("=== session boundary F8 count=%ld ===", (long)counter);
        log_line(
            "StrategySession active=%s desc='%s' throwRecall=%d dialogue=%d",
            subtitle_strategy_name(active_strategy),
            subtitle_strategy_desc(active_strategy),
            throw_recall_mute ? 1 : 0,
            dialogue_mute ? 1 : 0);
    }

    if (key_control_down[HOTKEY_CONTROL_CLEAR_LOG] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_CLEAR_LOG]) {
        clear_log_file();
        reset_log_capture_state();
        log_line("=== log cleared by F9; runtime hit budgets and probe caches reset ===");
    }

    EnterCriticalSection(&g_hotkey_lock);

    if (key_control_down[HOTKEY_CONTROL_THROW_RECALL_ON] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_THROW_RECALL_ON]) {
        g_hotkey_throw_recall_mute = TRUE;
        log_hotkey_mute_state("J");
    }
    if (key_control_down[HOTKEY_CONTROL_THROW_RECALL_OFF] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_THROW_RECALL_OFF]) {
        g_hotkey_throw_recall_mute = FALSE;
        log_hotkey_mute_state("K");
    }
    if (key_control_down[HOTKEY_CONTROL_DIALOGUE_ON] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_DIALOGUE_ON]) {
        g_hotkey_dialogue_mute = TRUE;
        log_hotkey_mute_state("N");
    }
    if (key_control_down[HOTKEY_CONTROL_DIALOGUE_OFF] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_DIALOGUE_OFF]) {
        g_hotkey_dialogue_mute = FALSE;
        log_hotkey_mute_state("M");
    }
    if (key_control_down[HOTKEY_CONTROL_CLEAR_ALL] &&
        !g_hotkey_control_prev[HOTKEY_CONTROL_CLEAR_ALL]) {
        g_hotkey_throw_recall_mute = FALSE;
        g_hotkey_dialogue_mute = FALSE;
        log_hotkey_mute_state("F12");
    }
    for (i = 0; i < SUBTITLE_STRATEGY_COUNT; ++i) {
        if (key_strategy_down[i] && !g_hotkey_strategy_prev[i]) {
            g_active_subtitle_strategy = i;
            log_line(
                "HotkeyStrategy active=%s key='F%u' desc='%s'",
                subtitle_strategy_name(i),
                (unsigned int)(i + 1u),
                subtitle_strategy_desc(i));
        }
    }
    LeaveCriticalSection(&g_hotkey_lock);

    for (i = 0; i < HOTKEY_CONTROL_COUNT; ++i) {
        g_hotkey_control_prev[i] = key_control_down[i];
    }
    for (i = 0; i < SUBTITLE_STRATEGY_COUNT; ++i) {
        g_hotkey_strategy_prev[i] = key_strategy_down[i];
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
    BOOL blocked_sender_only = g_cfg.enabled && should_block_sender_only_event_id(event_id);
    BOOL blocked = blocked_legacy || blocked_sender_only;
    const char *block_mode = blocked_sender_only ? "sender-only-narrow" :
                             (blocked_legacy ? "legacy" : "none");
    uintptr_t caller_ra = get_return_address_value();
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
        ? (caller_ra - g_image_base)
        : 0;
    uintptr_t ext_ptr = (uintptr_t)external_sources;
    uint64_t ext0 = safe_read_u64(ext_ptr + 0x0);
    uint64_t ext1 = safe_read_u64(ext_ptr + 0x8);
    uint64_t dedupe_key = 0;

    if (probe_enabled) {
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

static uintptr_t __fastcall hook_dollman_radio_play_voice_by_controller_delay(
    uintptr_t instance,
    int controller_index)
{
    if (is_legacy_dollman_radio_mute_enabled()) {
        log_line(
            "Muted DollmanRadio.PlayVoiceByControllerDelay instance=%p controller=%d",
            (void *)instance,
            controller_index);
        return 0;
    }

    return g_real_dollman_radio_play_voice_by_controller_delay(instance, controller_index);
}

static char __fastcall hook_voice_queue_submit(
    uintptr_t *queue_obj,
    unsigned int *request,
    char a3,
    uintptr_t a4,
    uintptr_t a5,
    unsigned char *a6)
{
    BOOL probe_enabled =
        (g_cfg.enable_deep_probe ||
         is_sender_only_dollman_radio_mute_enabled()) &&
        is_stf_probe_window_open();
    uintptr_t caller_ra = get_return_address_value();
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
        ? (caller_ra - g_image_base)
        : 0;

    if (probe_enabled && request != NULL && is_voice_queue_probe_caller(caller_rva)) {
        uintptr_t request_addr = (uintptr_t)request;
        uintptr_t request_ref = safe_read_ptr(request_addr + 0x8);
        uintptr_t request_ref_vtbl = safe_read_ptr(request_ref + 0x0);
        uintptr_t request_ref_vtbl_rva = 0;
        uintptr_t source_vtbl = safe_read_ptr(a4 + 0x0);
        uintptr_t source_vtbl_rva = 0;
        uintptr_t source_p20 = safe_read_ptr(a4 + 0x20);
        uintptr_t source_p20_vtbl = safe_read_ptr(source_p20 + 0x0);
        uintptr_t source_p20_vtbl_rva = 0;
        uintptr_t payload_vtbl = safe_read_ptr(a5 + 0x0);
        uintptr_t payload_vtbl_rva = 0;
        uint32_t request_key = safe_read_u32(request_addr + 0x0);
        int request_index = (int)safe_read_u32(request_addr + 0x10);
        uint32_t request_flag20 = safe_read_u32(request_addr + 0x14) & 0xFFu;
        uint32_t request_raw24 = safe_read_u32(request_addr + 0x18);
        uint32_t request_raw28 = safe_read_u32(request_addr + 0x1C);
        uint32_t request_tail_flags = safe_read_u32(request_addr + 0x20);
        uint32_t request_flag32 = request_tail_flags & 0xFFu;
        uint32_t request_flag33 = (request_tail_flags >> 8) & 0xFFu;
        uint32_t request_flag34 = (request_tail_flags >> 16) & 0xFFu;
        int request_param = (int)safe_read_u32(request_addr + 0x24);
        uint32_t source_mode_a0 = safe_read_u32(a4 + 0xA0);
        uint32_t source_p20_hi32 = read_identity_hi32(source_p20);
        uint32_t request_ref_tag = 0;
        uint32_t source_p20_tag = 0;
        uint32_t payload_tag = 0;
        char request_ref_text[160];
        char source_p20_text[160];
        char payload_text[160];
        BOOL request_ref_ok;
        BOOL source_p20_ok;
        BOOL payload_ok;
        uint64_t dedupe_key;

        ZeroMemory(request_ref_text, sizeof(request_ref_text));
        ZeroMemory(source_p20_text, sizeof(source_p20_text));
        ZeroMemory(payload_text, sizeof(payload_text));

        if (g_image_base != 0 && request_ref_vtbl > g_image_base) {
            request_ref_vtbl_rva = request_ref_vtbl - g_image_base;
        }
        if (g_image_base != 0 && source_vtbl > g_image_base) {
            source_vtbl_rva = source_vtbl - g_image_base;
        }
        if (g_image_base != 0 && source_p20_vtbl > g_image_base) {
            source_p20_vtbl_rva = source_p20_vtbl - g_image_base;
        }
        if (g_image_base != 0 && payload_vtbl > g_image_base) {
            payload_vtbl_rva = payload_vtbl - g_image_base;
        }

        request_ref_ok = read_localized_text_resource(
            request_ref,
            &request_ref_tag,
            request_ref_text,
            sizeof(request_ref_text));
        source_p20_ok = read_localized_text_resource(
            source_p20,
            &source_p20_tag,
            source_p20_text,
            sizeof(source_p20_text));
        payload_ok = read_localized_text_resource(
            a5,
            &payload_tag,
            payload_text,
            sizeof(payload_text));

        dedupe_key = ((uint64_t)caller_rva << 32) ^
                     ((uint64_t)request_key) ^
                     ((uint64_t)(uint32_t)request_index << 5) ^
                     ((uint64_t)request_raw24 << 13) ^
                     ((uint64_t)request_raw28 << 19) ^
                     ((uint64_t)(uint32_t)request_param << 27) ^
                     ((uint64_t)source_p20_hi32 << 7) ^
                     ((uint64_t)a5 >> 4) ^
                     0xDAC9100000000000ull;
        if (!stf_probe_seen_or_mark(dedupe_key)) {
            log_line(
                "[voice-queue] tid=%lu caller=%s caller_rva=0x%llx queue=0x%llx request=0x%llx "
                "req_key=0x%x req_ref=0x%llx req_ref_vtbl_rva=0x%llx req_ref_ok=%d req_ref_tag=0x%x req_ref_text=\"%s\" "
                "req_index=%d req_flag20=0x%x raw24=0x%x raw28=0x%x flags32_34=[0x%x,0x%x,0x%x] req_param=%d a3=0x%x "
                "source=0x%llx source_vtbl_rva=0x%llx source+0x20=0x%llx source20_vtbl_rva=0x%llx source20_hi32=0x%x source20_ok=%d source20_tag=0x%x source20_text=\"%s\" "
                "payload=0x%llx payload_vtbl_rva=0x%llx payload_ok=%d payload_tag=0x%x payload_text=\"%s\" source_mode_a0=0x%x",
                (unsigned long)GetCurrentThreadId(),
                voice_queue_probe_caller_name(caller_rva),
                (unsigned long long)caller_rva,
                (unsigned long long)queue_obj,
                (unsigned long long)request_addr,
                (unsigned int)request_key,
                (unsigned long long)request_ref,
                (unsigned long long)request_ref_vtbl_rva,
                request_ref_ok ? 1 : 0,
                (unsigned int)request_ref_tag,
                request_ref_text,
                request_index,
                (unsigned int)request_flag20,
                (unsigned int)request_raw24,
                (unsigned int)request_raw28,
                (unsigned int)request_flag32,
                (unsigned int)request_flag33,
                (unsigned int)request_flag34,
                request_param,
                (unsigned int)(unsigned char)a3,
                (unsigned long long)a4,
                (unsigned long long)source_vtbl_rva,
                (unsigned long long)source_p20,
                (unsigned long long)source_p20_vtbl_rva,
                (unsigned int)source_p20_hi32,
                source_p20_ok ? 1 : 0,
                (unsigned int)source_p20_tag,
                source_p20_text,
                (unsigned long long)a5,
                (unsigned long long)payload_vtbl_rva,
                payload_ok ? 1 : 0,
                (unsigned int)payload_tag,
                payload_text,
                (unsigned int)source_mode_a0);
        }
    }

    return g_real_voice_queue_submit(queue_obj, request, a3, a4, a5, a6);
}

static uintptr_t __fastcall hook_voice_shared_helper(
    uintptr_t manager_obj,
    uintptr_t voice_source,
    uintptr_t payload_source,
    int mode,
    unsigned int *sentence_key)
{
    BOOL probe_enabled =
        g_cfg.enable_deep_probe ||
        is_sender_only_dollman_radio_mute_enabled();

    if (probe_enabled && is_stf_probe_window_open()) {
        uintptr_t caller_ra = get_return_address_value();
        uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
            ? (caller_ra - g_image_base)
            : 0;
        const char *caller_name = "other";
        uintptr_t source_vtbl = safe_read_ptr(voice_source + 0x0);
        uintptr_t source_vtbl_rva = 0;
        uintptr_t source_p20 = safe_read_ptr(voice_source + 0x20);
        uintptr_t source_p20_vtbl = safe_read_ptr(source_p20 + 0x0);
        uintptr_t source_p20_vtbl_rva = 0;
        uintptr_t payload_vtbl = safe_read_ptr(payload_source + 0x0);
        uintptr_t payload_vtbl_rva = 0;
        uint32_t source_p20_hi32 = read_identity_hi32(source_p20);
        uint32_t source_mode_a0 = safe_read_u32(voice_source + 0xA0);
        uint32_t key = (sentence_key != NULL) ? *sentence_key : 0u;
        uint32_t payload_tag = 0;
        uint32_t source_p20_tag = 0;
        char payload_text[160];
        char source_p20_text[160];
        BOOL payload_ok;
        BOOL source_p20_ok;
        uint64_t dedupe_key;

        ZeroMemory(payload_text, sizeof(payload_text));
        ZeroMemory(source_p20_text, sizeof(source_p20_text));

        if (caller_rva == k_rva_voice_shared_helper_player_return) {
            caller_name = "player";
        } else if (caller_rva == k_rva_voice_shared_helper_dollman_return) {
            caller_name = "dollman";
        }

        if (g_image_base != 0 && source_vtbl > g_image_base) {
            source_vtbl_rva = source_vtbl - g_image_base;
        }
        if (g_image_base != 0 && source_p20_vtbl > g_image_base) {
            source_p20_vtbl_rva = source_p20_vtbl - g_image_base;
        }
        if (g_image_base != 0 && payload_vtbl > g_image_base) {
            payload_vtbl_rva = payload_vtbl - g_image_base;
        }

        payload_ok = read_localized_text_resource(
            payload_source,
            &payload_tag,
            payload_text,
            sizeof(payload_text));
        source_p20_ok = read_localized_text_resource(
            source_p20,
            &source_p20_tag,
            source_p20_text,
            sizeof(source_p20_text));

        dedupe_key = ((uint64_t)voice_source << 1) ^
                     ((uint64_t)payload_source >> 3) ^
                     ((uint64_t)(uint32_t)mode << 32) ^
                     ((uint64_t)caller_rva << 17) ^
                     (uint64_t)key ^
                     0xDAC7B00000000000ull;
        if (!stf_probe_seen_or_mark(dedupe_key)) {
            log_line(
                "[voice-shared] tid=%lu caller=%s caller_rva=0x%llx manager=0x%llx source=0x%llx source_vtbl_rva=0x%llx "
                "source+0x20=0x%llx source20_vtbl_rva=0x%llx source20_hi32=0x%x source20_ok=%d source20_tag=0x%x source20_text=\"%s\" "
                "payload=0x%llx payload_vtbl_rva=0x%llx payload_ok=%d payload_tag=0x%x payload_text=\"%s\" "
                "mode=%d mode_hex=0x%x key=0x%x source_mode_a0=0x%x",
                (unsigned long)GetCurrentThreadId(),
                caller_name,
                (unsigned long long)caller_rva,
                (unsigned long long)manager_obj,
                (unsigned long long)voice_source,
                (unsigned long long)source_vtbl_rva,
                (unsigned long long)source_p20,
                (unsigned long long)source_p20_vtbl_rva,
                (unsigned int)source_p20_hi32,
                source_p20_ok ? 1 : 0,
                (unsigned int)source_p20_tag,
                source_p20_text,
                (unsigned long long)payload_source,
                (unsigned long long)payload_vtbl_rva,
                payload_ok ? 1 : 0,
                (unsigned int)payload_tag,
                payload_text,
                mode,
                (unsigned int)mode,
                (unsigned int)key,
                (unsigned int)source_mode_a0);
        }
    }

    if (is_legacy_dollman_radio_mute_enabled()) {
        log_line(
            "Legacy voice shared-helper mute path reached manager=%p mode=%d key=0x%x payload=%p",
            (void *)manager_obj,
            mode,
            (sentence_key != NULL) ? (unsigned int)*sentence_key : 0u,
            (void *)payload_source);
    }

    return g_real_voice_shared_helper(
        manager_obj,
        voice_source,
        payload_source,
        mode,
        sentence_key);
}

static void __fastcall hook_dollman_voice_delay_closure(void *closure_state)
{
    log_dollman_voice_closure_probe(
        is_sender_only_dollman_radio_mute_enabled() ? "mute" : "pass",
        (uintptr_t)closure_state);

    if (is_sender_only_dollman_radio_mute_enabled()) {
        log_line(
            "Muted Dollman voice closure state=%p",
            closure_state);
        return;
    }

    g_real_dollman_voice_delay_closure(closure_state);
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
    if (g_cfg.enabled && g_cfg.enable_selector_probe && is_stf_probe_window_open() &&
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

static uintptr_t __fastcall hook_subtitle_runtime_wrapper(uintptr_t view, uintptr_t arg2)
{
    uintptr_t caller_ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                               ? (caller_ra - g_image_base)
                               : 0;
    SubtitleRuntimePrepareFn prepare_fn = (SubtitleRuntimePrepareFn)resolve_rva(k_rva_subtitle_prepare);
    SubtitleRuntimeRenderFn render_fn = (SubtitleRuntimeRenderFn)resolve_rva(k_rva_subtitle_render);
    SubtitleRuntimeStageFn stage_fn = NULL;
    SubtitleRuntimePayloadGetterFn payload_getter_fn = NULL;
    uintptr_t view_vtbl = 0;
    uintptr_t state = 0;
    uintptr_t state_vtbl = 0;
    uintptr_t payload_src = 0;
    uint32_t token = 0;
    uint8_t scratch[104];
    uint8_t meta[16];

    if (prepare_fn == NULL || render_fn == NULL || view == 0) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "bootstrap");
    }

    view_vtbl = safe_read_ptr(view);
    if (view_vtbl == 0 || IsBadReadPtr((const void *)(view_vtbl + 80), sizeof(uintptr_t))) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "view-vtbl");
    }
    stage_fn = (SubtitleRuntimeStageFn)safe_read_ptr(view_vtbl + 80);
    if (stage_fn == NULL) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "stage-fn");
    }

    token = read_subtitle_runtime_prepare_token();
    if (token == 0) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "prepare-token");
    }

    state = safe_read_ptr(view + 24);
    state_vtbl = safe_read_ptr(state);
    if (state == 0 || state_vtbl == 0 || IsBadReadPtr((const void *)(state_vtbl + 64), sizeof(uintptr_t))) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "state-pre-prepare");
    }
    payload_getter_fn = (SubtitleRuntimePayloadGetterFn)safe_read_ptr(state_vtbl + 64);
    if (payload_getter_fn == NULL) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "payload-getter-pre-prepare");
    }

    prepare_fn(view, arg2, token);

    state = safe_read_ptr(view + 24);
    state_vtbl = safe_read_ptr(state);
    if (state == 0 || state_vtbl == 0 || IsBadReadPtr((const void *)(state_vtbl + 64), sizeof(uintptr_t))) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "state-post-prepare");
    }

    stage_fn(view,
             state,
             view + 2300,
             view + 132,
             view + (16 * sizeof(uintptr_t)),
             view + (299 * sizeof(uintptr_t)));

    state = safe_read_ptr(view + 24);
    state_vtbl = safe_read_ptr(state);
    if (state == 0 || state_vtbl == 0 || IsBadReadPtr((const void *)(state_vtbl + 64), sizeof(uintptr_t))) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "state-post-stage");
    }

    payload_getter_fn = (SubtitleRuntimePayloadGetterFn)safe_read_ptr(state_vtbl + 64);
    if (payload_getter_fn == NULL) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "payload-getter-post-stage");
    }

    ZeroMemory(scratch, sizeof(scratch));
    ZeroMemory(meta, sizeof(meta));
    payload_src = payload_getter_fn(state, scratch, meta);
    if (payload_src == 0 || IsBadReadPtr((const void *)payload_src, 0x60)) {
        return pass_through_subtitle_runtime_wrapper(view, arg2, "payload-src");
    }

    if (process_subtitle_payload("wrapper", caller_rva, (const uint64_t *)payload_src)) {
        return 0;
    }

    memcpy((void *)(view + 0x20), (const void *)payload_src, 0x60);
    render_fn(view);
    return 0;
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

    if (hit_index <= 24) {
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

static void log_localized_hits_in_block(
    uintptr_t caller_rva,
    const char *label,
    uintptr_t base,
    size_t size)
{
    uint64_t words[16];
    size_t word_count;
    size_t i;

    if (!is_stf_probe_window_open()) {
        return;
    }
    if (base == 0 || size == 0 || size > sizeof(words) ||
        IsBadReadPtr((const void *)base, size)) {
        return;
    }

    ZeroMemory(words, sizeof(words));
    memcpy(words, (const void *)base, size);
    word_count = size / sizeof(words[0]);

    for (i = 0; i < word_count; ++i) {
        uint32_t tag = 0;
        char text[160];
        ZeroMemory(text, sizeof(text));
        if (read_localized_text_resource((uintptr_t)words[i], &tag, text, sizeof(text))) {
            log_line(
                "[350c70-hit] caller_rva=0x%llx %s+0x%x ptr=0x%llx tag=0x%x text=\"%s\"",
                (unsigned long long)caller_rva,
                label != NULL ? label : "?",
                (unsigned int)(i * sizeof(uint64_t)),
                (unsigned long long)(uintptr_t)words[i],
                (unsigned int)tag,
                text);
        }
    }
}

static const char *subtitle_strategy_name(uint32_t strategy)
{
    if (strategy < SUBTITLE_STRATEGY_COUNT) {
        return k_subtitle_strategy_meta[strategy].name;
    }
    return "unknown";
}

static const char *subtitle_strategy_desc(uint32_t strategy)
{
    if (strategy < SUBTITLE_STRATEGY_COUNT) {
        return k_subtitle_strategy_meta[strategy].desc;
    }
    return "unknown";
}

static void log_sentence_desc_probe(
    const char *tag,
    uintptr_t caller_rva,
    unsigned int index,
    uintptr_t desc)
{
    uintptr_t desc_vtbl;
    uintptr_t desc_vtbl_rva = 0;
    uintptr_t line_loc;
    uintptr_t speaker_wrap;
    uintptr_t speaker_wrap_vtbl;
    uintptr_t speaker_wrap_vtbl_rva = 0;
    uintptr_t speaker_loc;
    uint32_t line_tag = 0;
    uint32_t speaker_tag = 0;
    uint32_t speaker_wrap_tag = 0;
    char line_text[160];
    char speaker_text[160];
    BOOL line_ok;
    BOOL speaker_ok;

    if (!is_stf_probe_window_open() || desc == 0) {
        return;
    }

    ZeroMemory(line_text, sizeof(line_text));
    ZeroMemory(speaker_text, sizeof(speaker_text));

    desc_vtbl = safe_deref_qword(desc + 0x0);
    if (g_image_base != 0 && desc_vtbl > g_image_base) {
        desc_vtbl_rva = desc_vtbl - g_image_base;
    }

    line_loc = safe_deref_qword(desc + 0x48);
    speaker_wrap = safe_deref_qword(desc + 0x50);
    speaker_wrap_vtbl = safe_deref_qword(speaker_wrap + 0x0);
    if (g_image_base != 0 && speaker_wrap_vtbl > g_image_base) {
        speaker_wrap_vtbl_rva = speaker_wrap_vtbl - g_image_base;
    }
    speaker_wrap_tag = read_identity_hi32(speaker_wrap);
    speaker_loc = safe_deref_qword(speaker_wrap + 0x28);

    line_ok = read_localized_text_resource(line_loc, &line_tag, line_text, sizeof(line_text));
    speaker_ok = read_localized_text_resource(speaker_loc, &speaker_tag, speaker_text, sizeof(speaker_text));

    log_line(
        "[%s] caller_rva=0x%llx idx=%u desc=0x%llx desc_vtbl_rva=0x%llx "
        "line=0x%llx ok=%d tag=0x%x text=\"%s\" "
        "speaker_wrap=0x%llx wrap_vtbl_rva=0x%llx wrap_tag=0x%x "
        "speaker=0x%llx ok=%d tag=0x%x text=\"%s\"",
        tag != NULL ? tag : "desc",
        (unsigned long long)caller_rva,
        index,
        (unsigned long long)desc,
        (unsigned long long)desc_vtbl_rva,
        (unsigned long long)line_loc,
        line_ok ? 1 : 0,
        (unsigned int)line_tag,
        line_text,
        (unsigned long long)speaker_wrap,
        (unsigned long long)speaker_wrap_vtbl_rva,
        (unsigned int)speaker_wrap_tag,
        (unsigned long long)speaker_loc,
        speaker_ok ? 1 : 0,
        (unsigned int)speaker_tag,
        speaker_text);
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

    if (!is_stf_probe_window_open()) {
        return;
    }
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
    uintptr_t p200;
    uintptr_t desc;
    uintptr_t desc_vtbl;
    uintptr_t desc_vtbl_rva = 0;
    uint64_t pack_key0;
    uint64_t pack_key1;
    uint32_t pack_mode88;
    uintptr_t pack_builder96;
    uintptr_t pack_builder96_vtbl;
    uintptr_t pack_builder96_vtbl_rva = 0;
    uint32_t pack_flags104;
    uint32_t pack_aux108;
    uintptr_t pack_key112;
    uint64_t pack_key112_w0;
    uint64_t pack_key112_w1;
    uint64_t pack_pair120_0;
    uint64_t pack_pair120_1;
    uintptr_t pack_obj136;
    uintptr_t pack_obj136_vtbl;
    uintptr_t pack_obj136_vtbl_rva = 0;
    uint32_t pack_u144;
    uint32_t pack_u148;
    uintptr_t line_loc;
    uintptr_t speaker_wrap;
    uintptr_t speaker_wrap_vtbl;
    uintptr_t speaker_wrap_vtbl_rva = 0;
    uintptr_t speaker_loc;
    uint32_t line_tag = 0;
    uint32_t speaker_tag = 0;
    uint32_t speaker_wrap_tag = 0;
    char line_text[160];
    char speaker_text[160];
    BOOL line_ok;
    BOOL speaker_ok;
    uint64_t dedupe_key;
    uint64_t phase_hash;
    const unsigned char *phase_ptr;

    if (!is_stf_probe_window_open() || this_obj == 0) {
        return;
    }

    ZeroMemory(line_text, sizeof(line_text));
    ZeroMemory(speaker_text, sizeof(speaker_text));

    p200 = safe_deref_qword(this_obj + 200);
    desc = safe_deref_qword(p200);
    if (desc == 0) {
        return;
    }

    desc_vtbl = safe_deref_qword(desc + 0x0);
    if (g_image_base != 0 && desc_vtbl > g_image_base) {
        desc_vtbl_rva = desc_vtbl - g_image_base;
    }

    pack_key0 = safe_read_u64(this_obj + 48);
    pack_key1 = safe_read_u64(this_obj + 56);
    pack_mode88 = safe_read_u32(this_obj + 88);
    pack_builder96 = safe_read_ptr(this_obj + 96);
    pack_builder96_vtbl = safe_read_ptr(pack_builder96 + 0x0);
    if (g_image_base != 0 && pack_builder96_vtbl > g_image_base) {
        pack_builder96_vtbl_rva = pack_builder96_vtbl - g_image_base;
    }
    pack_flags104 = safe_read_u32(this_obj + 104);
    pack_aux108 = safe_read_u32(this_obj + 108);
    pack_key112 = safe_read_ptr(this_obj + 112);
    pack_key112_w0 = safe_read_u64(pack_key112 + 0x0);
    pack_key112_w1 = safe_read_u64(pack_key112 + 0x8);
    pack_pair120_0 = safe_read_u64(this_obj + 120);
    pack_pair120_1 = safe_read_u64(this_obj + 128);
    pack_obj136 = safe_read_ptr(this_obj + 136);
    pack_obj136_vtbl = safe_read_ptr(pack_obj136 + 0x0);
    if (g_image_base != 0 && pack_obj136_vtbl > g_image_base) {
        pack_obj136_vtbl_rva = pack_obj136_vtbl - g_image_base;
    }
    pack_u144 = safe_read_u32(this_obj + 144);
    pack_u148 = safe_read_u32(this_obj + 148);

    line_loc = safe_deref_qword(desc + 0x48);
    speaker_wrap = safe_deref_qword(desc + 0x50);
    speaker_wrap_vtbl = safe_deref_qword(speaker_wrap + 0x0);
    if (g_image_base != 0 && speaker_wrap_vtbl > g_image_base) {
        speaker_wrap_vtbl_rva = speaker_wrap_vtbl - g_image_base;
    }
    speaker_wrap_tag = read_identity_hi32(speaker_wrap);
    speaker_loc = safe_deref_qword(speaker_wrap + 0x28);

    line_ok = read_localized_text_resource(line_loc, &line_tag, line_text, sizeof(line_text));
    speaker_ok = read_localized_text_resource(speaker_loc, &speaker_tag, speaker_text, sizeof(speaker_text));

    dedupe_key = ((uint64_t)desc << 1) ^ ((uint64_t)line_loc >> 3) ^ ((uint64_t)speaker_loc << 7);
    dedupe_key ^= ((uint64_t)pack_mode88 << 32) ^ (uint64_t)pack_flags104;
    phase_hash = 1469598103934665603ull;
    for (phase_ptr = (const unsigned char *)(phase != NULL ? phase : "");
         *phase_ptr != '\0';
         ++phase_ptr) {
        phase_hash ^= (uint64_t)(*phase_ptr);
        phase_hash *= 1099511628211ull;
    }
    dedupe_key ^= phase_hash;
    if (stf_probe_seen_or_mark(dedupe_key)) {
        return;
    }

    log_line(
        "[stf-%s] tid=%lu this=0x%llx p200=0x%llx desc=0x%llx desc_vtbl_rva=0x%llx "
        "pack48=[0x%llx,0x%llx] mode88=0x%x builder96=0x%llx builder96_vtbl_rva=0x%llx "
        "flags104=0x%x aux108=0x%x key112=0x%llx key112_words=[0x%llx,0x%llx] "
        "pair120=[0x%llx,0x%llx] obj136=0x%llx obj136_vtbl_rva=0x%llx u144=0x%x u148=0x%x "
        "line=0x%llx ok=%d tag=0x%x text=\"%s\" "
        "speaker_wrap=0x%llx wrap_vtbl_rva=0x%llx wrap_tag=0x%x "
        "speaker=0x%llx ok=%d tag=0x%x text=\"%s\"",
        phase != NULL ? phase : "?",
        (unsigned long)GetCurrentThreadId(),
        (unsigned long long)this_obj,
        (unsigned long long)p200,
        (unsigned long long)desc,
        (unsigned long long)desc_vtbl_rva,
        (unsigned long long)pack_key0,
        (unsigned long long)pack_key1,
        (unsigned int)pack_mode88,
        (unsigned long long)pack_builder96,
        (unsigned long long)pack_builder96_vtbl_rva,
        (unsigned int)pack_flags104,
        (unsigned int)pack_aux108,
        (unsigned long long)pack_key112,
        (unsigned long long)pack_key112_w0,
        (unsigned long long)pack_key112_w1,
        (unsigned long long)pack_pair120_0,
        (unsigned long long)pack_pair120_1,
        (unsigned long long)pack_obj136,
        (unsigned long long)pack_obj136_vtbl_rva,
        (unsigned int)pack_u144,
        (unsigned int)pack_u148,
        (unsigned long long)line_loc,
        line_ok ? 1 : 0,
        (unsigned int)line_tag,
        line_text,
        (unsigned long long)speaker_wrap,
        (unsigned long long)speaker_wrap_vtbl_rva,
        (unsigned int)speaker_wrap_tag,
        (unsigned long long)speaker_loc,
        speaker_ok ? 1 : 0,
        (unsigned int)speaker_tag,
        speaker_text);
}

static void log_dollman_voice_closure_probe(const char *phase, uintptr_t closure_state)
{
    BOOL probe_enabled =
        (g_cfg.enable_deep_probe ||
         is_sender_only_dollman_radio_mute_enabled()) &&
        is_stf_probe_window_open();
    uintptr_t source_obj;
    uintptr_t source_vtbl;
    uintptr_t source_vtbl_rva = 0;
    uintptr_t source_p20;
    uintptr_t source_p20_vtbl;
    uintptr_t source_p20_vtbl_rva = 0;
    uint32_t source_p20_hi32;
    uint32_t captured_id;
    uint32_t source_mode_a0;
    uint64_t dedupe_key;

    if (!probe_enabled || closure_state == 0) {
        return;
    }

    source_obj = safe_read_ptr(closure_state + 0x0);
    if (source_obj == 0) {
        return;
    }

    captured_id = safe_read_u32(closure_state + 0x8);
    source_vtbl = safe_read_ptr(source_obj + 0x0);
    if (g_image_base != 0 && source_vtbl > g_image_base) {
        source_vtbl_rva = source_vtbl - g_image_base;
    }

    source_p20 = safe_read_ptr(source_obj + 0x20);
    source_p20_vtbl = safe_read_ptr(source_p20 + 0x0);
    if (g_image_base != 0 && source_p20_vtbl > g_image_base) {
        source_p20_vtbl_rva = source_p20_vtbl - g_image_base;
    }
    source_p20_hi32 = read_identity_hi32(source_p20);
    source_mode_a0 = safe_read_u32(source_obj + 0xA0);

    dedupe_key = ((uint64_t)source_obj << 1) ^
                 ((uint64_t)source_p20 >> 3) ^
                 ((uint64_t)captured_id << 32) ^
                 ((uint64_t)source_mode_a0 << 11) ^
                 0xD011A4D0A11B00B5ull;
    if (phase != NULL && phase[0] == 'p') {
        dedupe_key ^= 0x9E3779B97F4A7C15ull;
    }
    if (stf_probe_seen_or_mark(dedupe_key)) {
        return;
    }

    log_line(
        "[voice-%s] tid=%lu closure=0x%llx source=0x%llx source_vtbl_rva=0x%llx "
        "source+0x20=0x%llx p20_vtbl_rva=0x%llx p20_hi32=0x%x "
        "captured_id=0x%x source_mode_a0=0x%x",
        phase != NULL ? phase : "?",
        (unsigned long)GetCurrentThreadId(),
        (unsigned long long)closure_state,
        (unsigned long long)source_obj,
        (unsigned long long)source_vtbl_rva,
        (unsigned long long)source_p20,
        (unsigned long long)source_p20_vtbl_rva,
        (unsigned int)source_p20_hi32,
        (unsigned int)captured_id,
        (unsigned int)source_mode_a0);

    log_localized_text_resource_candidate("voice-source+0x20", source_p20);
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
    uintptr_t previous_current_family = SUBTITLE_FAMILY_NONE;
    uint32_t family = SUBTITLE_FAMILY_NONE;
    uintptr_t result;
    BOOL probe_enabled = (g_cfg.enable_subtitle_producer_probe ||
                          g_cfg.enable_builder_probe ||
                          g_cfg.enable_deep_probe);

    if (probe_enabled) {
        log_start_talk_function_snapshot("pre", this_obj);
    }

    if (this_obj != 0) {
        pre_p200 = safe_deref_qword(this_obj + 200);
        pre_p200_deref = safe_deref_qword(pre_p200);
        pre_p200_field72 = safe_deref_qword(pre_p200_deref + 72);
        family = classify_subtitle_family(pre_p200_field72);
        previous_current_family = tls_get_current_subtitle_family();
        tls_set_current_subtitle_family((uintptr_t)family);
    }

    result = g_real_subtitle_producer(this_obj);

    if (probe_enabled) {
        log_start_talk_function_snapshot("post", this_obj);
    }

    if (this_obj != 0) {
        tls_set_current_subtitle_family(previous_current_family);
    }

    return result;
}

static uintptr_t __fastcall hook_start_talk_init(uintptr_t this_obj)
{
    uintptr_t result;
    BOOL probe_enabled = (g_cfg.enable_subtitle_producer_probe ||
                          g_cfg.enable_builder_probe ||
                          g_cfg.enable_deep_probe);

    if (probe_enabled) {
        log_start_talk_function_snapshot("sti-pre", this_obj);
    }

    result = g_real_start_talk_init(this_obj);

    if (probe_enabled) {
        log_start_talk_function_snapshot("sti-post", this_obj);
    }

    return result;
}

static uintptr_t __fastcall hook_gameplay_sink(
    uintptr_t rcx,
    uintptr_t rdx,
    uintptr_t r8,
    uintptr_t r9,
    uintptr_t a5,
    uintptr_t a6,
    uintptr_t a7,
    uintptr_t a8,
    uintptr_t a9)
{
    uintptr_t caller_ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t caller_rva = (g_image_base != 0 && caller_ra > g_image_base)
                               ? (caller_ra - g_image_base)
                               : 0;
    uint64_t dedupe_key;

    if (g_cfg.enable_builder_probe && is_stf_probe_window_open()) {
        uint32_t span_count = 0;
        uintptr_t span_data = 0;
        uint64_t candidate0[5];
        unsigned int i;

        dedupe_key = 0x350C70ull ^
                     ((uint64_t)caller_rva << 1) ^
                     ((uint64_t)(rdx >> 4)) ^
                     ((uint64_t)(a6 >> 5)) ^
                     ((uint64_t)(a7 >> 6)) ^
                     ((uint64_t)((uint32_t)a5) << 32) ^
                     (uint64_t)(uint32_t)a9;
        if (!stf_probe_seen_or_mark(dedupe_key)) {
            ZeroMemory(candidate0, sizeof(candidate0));
            if (a6 != 0 && !IsBadReadPtr((const void *)a6, 16)) {
                span_count = *(const uint32_t *)(a6 + 0x0);
                span_data = *(const uintptr_t *)(a6 + 0x8);
                if (span_count != 0 && span_data != 0 &&
                    !IsBadReadPtr((const void *)span_data, sizeof(candidate0))) {
                    memcpy(candidate0, (const void *)span_data, sizeof(candidate0));
                }
            }

            log_line(
                "[350c70] tid=%lu caller_rva=0x%llx this=0x%llx arg2=0x%llx arg3=0x%x arg4=0x%llx "
                "arg5=0x%x arg6=0x%llx {count=%u data=0x%llx c0=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]} "
                "arg7=0x%llx arg8=0x%x arg9=0x%x",
                (unsigned long)GetCurrentThreadId(),
                (unsigned long long)caller_rva,
                (unsigned long long)rcx,
                (unsigned long long)rdx,
                (unsigned int)(uint32_t)r8,
                (unsigned long long)r9,
                (unsigned int)(uint32_t)a5,
                (unsigned long long)a6,
                (unsigned int)span_count,
                (unsigned long long)span_data,
                (unsigned long long)candidate0[0],
                (unsigned long long)candidate0[1],
                (unsigned long long)candidate0[2],
                (unsigned long long)candidate0[3],
                (unsigned long long)candidate0[4],
                (unsigned long long)a7,
                (unsigned int)(uint8_t)a8,
                (unsigned int)(uint32_t)a9);

            log_localized_hits_in_block(caller_rva, "arg2", rdx, 0x30);
            log_localized_hits_in_block(caller_rva, "arg7", a7, 0x50);
            if (r9 != 0) {
                uint32_t tag = 0;
                char text[160];
                ZeroMemory(text, sizeof(text));
                if (read_localized_text_resource(r9, &tag, text, sizeof(text))) {
                    log_line(
                        "[350c70-hit] caller_rva=0x%llx arg4 ptr=0x%llx tag=0x%x text=\"%s\"",
                        (unsigned long long)caller_rva,
                        (unsigned long long)r9,
                        (unsigned int)tag,
                        text);
                }
            }

            if (span_count != 0 && span_data != 0) {
                unsigned int max_entries = span_count;
                if (max_entries > 4u) {
                    max_entries = 4u;
                }
                for (i = 0; i < max_entries; ++i) {
                    uintptr_t desc = safe_deref_qword(span_data + (uintptr_t)i * 0x28u);
                    log_sentence_desc_probe("350c70-desc", caller_rva, i, desc);
                }
            }
        }
    }

    return g_real_gameplay_sink(rcx, rdx, r8, r9, a5, a6, a7, a8, a9);
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

    if (g_cfg.enable_builder_probe && is_stf_probe_window_open()) {
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

    if (g_cfg.enable_selector_probe && is_stf_probe_window_open()) {
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

static uintptr_t pass_through_subtitle_runtime_wrapper(
    uintptr_t view,
    uintptr_t arg2,
    const char *reason)
{
    log_verbose(
        "[wrapper-pass] reason=%s view=0x%llx arg2=0x%llx",
        reason != NULL ? reason : "?",
        (unsigned long long)view,
        (unsigned long long)arg2);
    if (g_real_subtitle_runtime_wrapper == NULL) {
        return 0;
    }
    return g_real_subtitle_runtime_wrapper(view, arg2);
}

__declspec(dllexport) int core_init(const ProxyContext *ctx)
{
    MH_STATUS status;
    unsigned int hook_count = 0;
    BOOL throw_recall_mute = FALSE;
    BOOL dialogue_mute = FALSE;
    BOOL need_show_subtitle_hook = FALSE;
    BOOL need_subtitle_producer_hook = FALSE;
    BOOL show_subtitle_hook_installed = FALSE;
    BOOL subtitle_runtime_surface_enabled = FALSE;
    BOOL sender_only_runtime_mode = FALSE;
    BOOL effective_dollman_radio_mute = FALSE;
    BOOL sender_only_dollman_voice_mute = FALSE;
    BOOL install_subtitle_runtime_wrapper = FALSE;
    BOOL need_deep_probe = FALSE;
    BOOL need_voice_dispatch_hook = FALSE;
    BOOL need_voice_closure_hook = FALSE;

    ZeroMemory(&g_proxy_ctx, sizeof(g_proxy_ctx));
    if (ctx != NULL) {
        g_proxy_ctx = *ctx;
    }

    InterlockedExchange(&g_core_shutting_down, 0);

    InitializeCriticalSection(&g_log_lock);
    g_log_lock_inited = TRUE;
    InitializeCriticalSection(&g_identity_lock);
    g_identity_lock_inited = TRUE;
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
    reset_builder_hit_counts();
    reset_strategy_stats();
    reset_hotkey_runtime_state();
    reset_session_probe_state();
    reset_log_capture_state();
    seed_hotkey_state_from_config();
    get_hotkey_mute_state(&throw_recall_mute, &dialogue_mute, &g_active_subtitle_strategy);
    subtitle_runtime_surface_enabled = g_cfg.enable_subtitle_runtime_hooks;
    sender_only_runtime_mode = is_sender_only_runtime_mode_enabled();
    sender_only_dollman_voice_mute = is_sender_only_dollman_radio_mute_enabled();
    effective_dollman_radio_mute = is_legacy_dollman_radio_mute_enabled();
    install_subtitle_runtime_wrapper =
        subtitle_runtime_surface_enabled &&
        !sender_only_runtime_mode &&
        g_cfg.enable_legacy_runtime_wrapper;
    need_deep_probe = g_cfg.enable_deep_probe;
    need_voice_dispatch_hook =
        sender_only_dollman_voice_mute ||
        need_deep_probe;
    need_voice_closure_hook = sender_only_dollman_voice_mute || need_deep_probe;

    log_line("DollmanMute build: %s", k_build_tag);
    log_line("DollmanMute image_base=0x%llx image_size=0x%llx", (unsigned long long)g_image_base, (unsigned long long)g_image_size);
    log_line(
        "DollmanMute init start: enabled=%d verbose=%d dollmanRadioMute=%d effectiveRadioMute=%d senderOnlyVoiceMute=%d throwRecallSubtitleMute=%d activeStrategy=%s strategyDesc='%s' subtitleRuntime=%d senderOnlyMode=%d legacyWrapper=%d familyTracking=%d producerProbe=%d builderProbe=%d selectorProbe=%d deepProbe=%d talkProbe=%d scannerMode=%u",
        g_cfg.enabled,
        g_cfg.verbose_log,
        g_cfg.enable_dollman_radio_mute,
        effective_dollman_radio_mute,
        sender_only_dollman_voice_mute,
        g_cfg.enable_throw_recall_subtitle_mute,
        subtitle_strategy_name(g_active_subtitle_strategy),
        subtitle_strategy_desc(g_active_subtitle_strategy),
        subtitle_runtime_surface_enabled,
        sender_only_runtime_mode,
        g_cfg.enable_legacy_runtime_wrapper,
        g_cfg.enable_subtitle_family_tracking,
        g_cfg.enable_subtitle_producer_probe,
        g_cfg.enable_builder_probe,
        g_cfg.enable_selector_probe,
        g_cfg.enable_deep_probe,
        g_cfg.enable_talk_dispatcher_probe,
        (unsigned int)g_cfg.scanner_mode);

    need_show_subtitle_hook =
        subtitle_runtime_surface_enabled ||
        g_cfg.enable_builder_probe;
    need_subtitle_producer_hook =
        g_cfg.enable_subtitle_producer_probe ||
        g_cfg.enable_builder_probe ||
        need_deep_probe ||
        g_cfg.enable_subtitle_family_tracking;

    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("MH_Initialize failed: %d", (int)status);
        return 0;
    }

    if (effective_dollman_radio_mute ||
        g_cfg.scanner_mode != SCANNER_MODE_OFF ||
        sender_only_dollman_voice_mute ||
        need_deep_probe) {
        if (install_export_hook(
                k_export_post_event_id,
                hook_post_event_id,
                (void **)&g_real_post_event_id,
                "PostEventID")) {
            ++hook_count;
            if (!effective_dollman_radio_mute && g_cfg.scanner_mode == SCANNER_MODE_OFF) {
                if (sender_only_dollman_voice_mute) {
                    log_line(
                        "PostEventID sender-only narrow mute active: throw/recall Wwise events are blocked; F8 windows also log audio events");
                } else if (need_deep_probe) {
                    log_line("PostEventID passive probe active: F8 windows log audio events without legacy broad mute");
                }
            }
        }
    } else {
        log_line("Legacy PostEvent mute path disabled");
    }

    if (effective_dollman_radio_mute) {
        if (install_rva_hook(
                k_rva_dollman_radio_play_voice_by_controller_delay,
                hook_dollman_radio_play_voice_by_controller_delay,
                (void **)&g_real_dollman_radio_play_voice_by_controller_delay,
                "DollmanRadio.PlayVoiceByControllerDelay")) {
            ++hook_count;
        }
    } else {
        if (g_cfg.enable_dollman_radio_mute && sender_only_runtime_mode) {
            log_line("Dollman radio mute suppressed by sender-only runtime mode");
        } else {
            log_line("Dollman radio mute disabled by config");
        }
    }

    if (need_voice_dispatch_hook) {
        if (install_rva_hook(
                k_rva_voice_queue_submit,
                hook_voice_queue_submit,
                (void **)&g_real_voice_queue_submit,
                "VoiceQueueSubmit.sub_140DAC910")) {
            ++hook_count;
        }
    } else {
        log_line("Voice queue submit hook disabled");
    }

    if (need_voice_dispatch_hook) {
        if (install_rva_hook(
                k_rva_voice_shared_helper,
                hook_voice_shared_helper,
                (void **)&g_real_voice_shared_helper,
                "VoiceSharedHelper.sub_140DAC7B0")) {
            ++hook_count;
        }
    } else {
        log_line("Voice shared-helper hook disabled");
    }

    if (need_voice_closure_hook) {
        if (install_rva_hook(
                k_rva_dollman_voice_delay_closure,
                hook_dollman_voice_delay_closure,
                (void **)&g_real_dollman_voice_delay_closure,
                "DollmanVoiceDelayClosure.sub_140C73EE0")) {
            ++hook_count;
            if (sender_only_dollman_voice_mute) {
                log_line("Sender-only Dollman voice mute active via closure.sub_140C73EE0");
            }
        }
    } else {
        log_line("Dollman voice closure hook disabled");
    }

    if (need_show_subtitle_hook) {
        if (install_subtitle_runtime_wrapper) {
            if (install_rva_hook(
                    k_rva_subtitle_runtime_wrapper,
                    hook_subtitle_runtime_wrapper,
                    (void **)&g_real_subtitle_runtime_wrapper,
                    "GameViewGame.SubtitleRuntime.sub_140780690")) {
                show_subtitle_hook_installed = TRUE;
                ++hook_count;
            } else {
                log_line("Subtitle runtime wrapper hook unavailable on this build");
            }
        } else {
            if (sender_only_runtime_mode) {
                log_line("Sender-only runtime mode active: skipping subtitle runtime wrapper hook");
            } else if (!g_cfg.enable_legacy_runtime_wrapper) {
                log_line("Legacy subtitle runtime wrapper disabled by config; sender surface remains active");
            }
        }

        if (install_rva_hook(
                k_rva_show_subtitle,
                hook_show_subtitle,
                (void **)&g_real_show_subtitle,
                "GameViewGame.ShowSubtitleSender.sub_140780740")) {
            show_subtitle_hook_installed = TRUE;
            ++hook_count;
        } else {
            log_line("Subtitle sender hook unavailable on this build");
        }

        if (install_rva_hook(
                k_rva_remove_subtitle,
                hook_remove_subtitle,
                (void **)&g_real_remove_subtitle,
                "GameViewGame.RemoveSubtitleSender.sub_140780840")) {
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

    if (need_subtitle_producer_hook) {
        if (install_rva_hook(
                k_rva_subtitle_producer,
                hook_subtitle_producer,
                (void **)&g_real_subtitle_producer,
                "StartTalkFunction.slot15.producer")) {
            ++hook_count;
        }
    } else {
        log_line("Subtitle producer hook disabled by config");
    }

    if (subtitle_runtime_surface_enabled &&
        subtitle_strategy_uses_family_tracking(g_active_subtitle_strategy) &&
        !g_cfg.enable_subtitle_family_tracking) {
        log_line("ShowSubtitle payload family classification active: producer-side family tracking not required for active strategy");
    }

    if (g_cfg.enable_subtitle_producer_probe || need_deep_probe) {
        if (install_rva_hook(
                k_rva_start_talk_init,
                hook_start_talk_init,
                (void **)&g_real_start_talk_init,
                "StartTalkFunction.init.sub_140387670")) {
            ++hook_count;
        }
    } else {
        log_line("Deep StartTalk init probe disabled (needs producerProbe or deepProbe)");
    }

    if (g_cfg.enable_builder_probe) {
        if (g_cfg.enable_subtitle_producer_probe && k_rva_gameplay_sink != 0) {
            if (install_rva_hook(
                    k_rva_gameplay_sink,
                    hook_gameplay_sink,
                    (void **)&g_real_gameplay_sink,
                    "GameplaySink.currentBuild")) {
                ++hook_count;
            }
        } else if (g_cfg.enable_subtitle_producer_probe) {
            log_line("GameplaySink interior hook quarantined on current build");
        }
        if (k_rva_builder_a != 0) {
            if (install_rva_hook(
                    k_rva_builder_a,
                    hook_builder_a,
                    (void **)&g_real_builder_a,
                    "BuilderA.currentBuild")) {
                ++hook_count;
            }
        } else {
            log_line("BuilderA hook quarantined on current build: unresolved function entry");
        }
        if (install_rva_hook(
                k_rva_builder_b,
                hook_builder_b,
                (void **)&g_real_builder_b,
                "BuilderB.sub_140350380")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_c,
                hook_builder_c,
                (void **)&g_real_builder_c,
                "BuilderC.sub_140350790")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_u1,
                hook_builder_u1,
                (void **)&g_real_builder_u1,
                "BuilderU1.sub_140B5BC60")) {
            ++hook_count;
        }
        if (install_rva_hook(
                k_rva_builder_u2,
                hook_builder_u2,
                (void **)&g_real_builder_u2,
                "BuilderU2.sub_140B5CC20")) {
            ++hook_count;
        }
        if (g_cfg.enable_subtitle_producer_probe) {
            if (k_rva_gameplay_sink != 0) {
                log_line("Builder probe armed: F8 = session boundary marker + 5s probe window, shared sink probe active");
            } else {
                log_line("Builder probe armed: F8 = session boundary marker + 5s probe window, shared sink probe quarantined on current build");
            }
        } else {
            log_line("Builder probe armed: F8 = session boundary marker + 5s probe window");
        }
    } else {
        log_line("Builder probe disabled by config");
    }

    if (g_cfg.enable_selector_probe) {
        if (install_rva_hook(
                k_rva_selector_dispatch,
                hook_selector_dispatch,
                (void **)&g_real_selector_dispatch,
                "SelectorDispatch.sub_140DAF8A0")) {
            ++hook_count;
        }
        log_line("Selector dispatch probe armed (sub_140DAF8A0)");
    } else {
        log_line("Selector dispatch probe disabled by config");
    }

    if (need_deep_probe) {
        log_line("Deep correlation probe armed: F8 window correlates StartTalk producer/init with Dollman voice hooks");
    } else {
        log_line("Deep correlation probe disabled by config");
    }

    if (g_cfg.enable_talk_dispatcher_probe) {
        log_line("TalkDispatcher probe remains quarantined on current build; not installed");
    } else {
        log_line("TalkDispatcher probe disabled by config");
    }

    g_hotkey_thread_handle = CreateThread(NULL, 0, hotkey_thread_proc, NULL, 0, NULL);
    if (g_hotkey_thread_handle != NULL) {
        log_line(
            "Hotkeys active: J=throwRecall on, K=throwRecall off, N=dialogue on, M=dialogue off, F12=clear, F1..F6=strategy (startup throwRecall=%d dialogue=%d activeStrategy=%s desc='%s')",
            throw_recall_mute ? 1 : 0,
            dialogue_mute ? 1 : 0,
            subtitle_strategy_name(g_active_subtitle_strategy),
            subtitle_strategy_desc(g_active_subtitle_strategy));
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

    log_line(
        "Builder probe hit summary: A=%ld B=%ld C=%ld U1=%ld U2=%ld sessions=%ld",
        (long)g_builder_hit_counts[BUILDER_ID_A],
        (long)g_builder_hit_counts[BUILDER_ID_B],
        (long)g_builder_hit_counts[BUILDER_ID_C],
        (long)g_builder_hit_counts[BUILDER_ID_U1],
        (long)g_builder_hit_counts[BUILDER_ID_U2],
        (long)g_session_counter);
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
    g_real_dollman_radio_play_voice_by_controller_delay = NULL;
    g_real_voice_shared_helper = NULL;
    g_real_voice_queue_submit = NULL;
    g_real_dollman_voice_delay_closure = NULL;
    g_real_subtitle_runtime_wrapper = NULL;
    g_real_show_subtitle = NULL;
    g_real_remove_subtitle = NULL;
    g_real_subtitle_producer = NULL;
    g_real_start_talk_init = NULL;
    g_real_gameplay_sink = NULL;
    g_real_builder_a = NULL;
    g_real_builder_b = NULL;
    g_real_builder_c = NULL;
    g_real_builder_u1 = NULL;
    g_real_builder_u2 = NULL;
    g_real_selector_dispatch = NULL;
    g_real_talk_dispatcher = NULL;

    if (g_tls_current_subtitle_family != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_current_subtitle_family);
        g_tls_current_subtitle_family = TLS_OUT_OF_INDEXES;
    }
    if (g_tls_last_builder != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_last_builder);
        g_tls_last_builder = TLS_OUT_OF_INDEXES;
    }

    g_identity_cache_count = 0;
    g_identity_cache_full_warned = FALSE;
    g_image_base = 0;
    reset_session_probe_state();
    reset_hotkey_runtime_state();

    log_line("core_shutdown complete");

    if (g_hotkey_lock_inited) {
        DeleteCriticalSection(&g_hotkey_lock);
        g_hotkey_lock_inited = FALSE;
    }
    if (g_stf_probe_lock_inited) {
        DeleteCriticalSection(&g_stf_probe_lock);
        g_stf_probe_lock_inited = FALSE;
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

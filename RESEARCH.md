# DollmanMute 字幕屏蔽研究笔记

> 最后更新: 2026-04-26
> 当前运行时底座: **research-v3.25c-subtitle-decode-fix**
> 历史深度研究主线: **v3.19+ (旧笔记保留在下方)**

## 0E. 2026-04-26 当前 live 突破与副作用

- 当前 build 的 `LocalizedTextResource` vtable 常量已经重新坐实：**当前 live 是 `0x3448D38`，不是旧文里的 `0x3448E48`**。之前 sender 路径下 `speaker_ok=0 / line_ok=0` 的直接原因就是这个常量漂移。
- 修正后，`ShowSubtitle` sender 路径已经重新能直接解出 `p6/p7`：当前 live 已看到
  - `p6 tag 0x1F4 -> "Alright, I'm coming in!"`
  - `p7 tag 0x12B6F -> "Dollman"`
  - 非目标样本也能正常解出，如 `Gen Hoshino` / `APAS` / `Fragile` / `Sam` / `Tarman`
- 当前 gameplay Dollman 精确 pair 也已经更新到 **`caller_rva=0x385C1B` + `speaker_tag=0x12B6F`**；旧 `0x38598B` 现在确认只是历史值。
- 这轮最关键的 live 证据是：`Muted subtitle surface=sender strategy=pair caller_rva=0x385c1b speaker_tag=0x12b6f line_tag=0x1f4 family=throwRecall` 已经出现。也就是说，**gameplay Dollman 字幕的 sender-side 精确静音第一次在当前 build 上被实锤命中**。
- 同时也必须把副作用记清楚：用户实机反馈显示，**当前实验 build 虽然屏蔽 gameplay 对话台词的效果极好，但主角也会变成“哑巴”**，按呼喊键时连动作都没有。这个症状说明当前方案虽然证明了字幕判定面是对的，但**整体运行时 blast radius 仍然过大，不能当成可交付状态**。
- 当前最合理的工作假设是：**字幕 sender pair 判定本身已经接近正确，而“主角也变哑巴 / 呼喊键无动作”更像别的运行时面仍在过度拦截**。这一条目前是待继续二分验证的主阻塞项，不要把“字幕命中成功”误判成“整个方案已经完成”。

## 0D. 2026-04-26 当前 live 纠偏

- 当前 live 工作构建已经切到 `research-v3.25b-subtitle-candidate-probe`；它不再只是 `selector-only` 平台，而是已经常驻 `sub_140780690 / sub_140780740 / sub_140780840` 这组当前 build 对齐后的字幕 runtime sender/remove 入口。
- 旧 `ShowSubtitle` 精确 pair 规则 `(speaker_tag=0x12B6F, caller_rva=0x38598B)` 现在必须降格成**历史结论**。当前 live log 已经稳定显示 gameplay sender caller 是 `0x385C1B`，paired remove caller 是 `0x385B32`。
- 当前最硬的 live 事实不是 `speaker_tag/line_tag`，而是 **sender `q2/q3` 与 remove `key0/key1` 一一对应**。当前样本值是 `0x0D4A974A2BF71653 / 0x836B97A035CBB78D`。
- 旧文里“`p6/p7` 可直接解码成 `LocalizedTextResource`”这一条，在旧 live 会话里是成立的；但**当前 build / 当前命中链路**下，runtime log 里 `p6_ok=0 / p7_ok=0`，所以这条结论现在只能按**历史结论，待重新验证**处理。
- 因此后续主线不要再把 `0x38598B`、`speaker_tag=0x12B6F`、`p6/p7` 直接解码，当成当前 build 的既成事实；它们要么已经漂移，要么至少尚未被这轮 live 重新坐实。

## 0A. 2026-04-25 运行时稳定化更新

- 当前用户目标已经从“继续堆 probe”切换成“先拿到一个不闪退的先进研究底座”。
- 当前稳定默认值已经落在 `research-v3.24g-stable-selector-platform`。
- 这版默认 **只装 `SelectorDispatch` (`0x00DAFAD0`)**。
- 这版默认 **不装 `TalkDispatcher` (`0x003857E0`)**，因为它已经被二分实锤为当前致崩钩子。
- 这版默认也不装 `ShowSubtitle`、`StartTalkFunction.slot15 producer`、`StartTalk init`、`GameplaySink 0x350C70`、`Builder A/B/C/U1/U2`。
- `F8` 只负责打开 5 秒 deep-probe window，不是防闪退开关。
- 当前游戏 build 下，`Failed to create hook ...: 8` = MinHook `MH_ERROR_UNSUPPORTED_FUNCTION`。当前已观测到 `ShowSubtitle` 和 `BuilderA` 直挂会返回这个状态。
- 当前代码里已新增 `EnableTalkDispatcherProbe=0`，后续 agent 不要再把 `SelectorDispatch` 和 `TalkDispatcher` 绑成同一个开关。
- 重要环境细节：`build.ps1` 不会覆盖已经存在的 game-root `DollmanMute.ini`。也就是说，新键位即使已经进了代码默认值，磁盘上的旧 ini 也可能还没有那一行，需要手动补。

## 0B. 运行时崩溃二分结论

| 版本 / build tag | 默认启用的运行时钩子 | 结果 | 结论 |
|---|---|---|---|
| `research-v3.24c-safe-boot` | 全关 | 不闪退 | 代理 / core 基座安全 |
| `research-v3.24d-half-a` | `producer + selector + talk` | 闪退 | 问题在这半边 |
| `research-v3.24e-selector-only` | `selector + talk` | 闪退 | `builder / producer` 不是必要条件 |
| `research-v3.24f-selector-dispatch-only` | `selector` only | 不闪退 | `SelectorDispatch` 可单独保留 |
| `research-v3.24g-stable-selector-platform` | `selector` only + `EnableTalkDispatcherProbe=0` | 不闪退 | 当前正式稳定底座 |

**因此当前唯一已经被实锤的 runtime crash trigger 是**:

- `TalkDispatcher` (历史 probe 点 `0x003857E0`，位于 `sub_140385720 + 0xC0`)

**当前唯一已经被实锤可单独保留的 selector 侧钩子是**:

- `SelectorDispatch` (历史 probe 点 `0x00DAFAD0`，位于 `sub_140DAF8A0 + 0x230`)

## 0C. 2026-04-26 新 IDA 对齐与地址卫生

- 当前打开的 IDA 数据库是 `C:\Users\Administrator\Downloads\dbg\ds2ida\DS2.exe.i64`，输入文件是 `C:\Users\Administrator\Downloads\dbg\ds2ida\DS2.exe`。
- 它已经和游戏目录里的 live `DS2.exe` 对齐：版本同为 `1.5.68.0`，SHA256 同为 `06B6FEC8074896747E34E95173E1C7931306CEA54C6B34182EE47C318427EA62`。
- 当前 `hexrays_ready=true`，但 `auto_analysis_ready=false`。这意味着新库已经能用来重建主链，但个别函数边界仍可能继续收敛。
- 过去文档里大量写法其实是“历史 probe 偏移 / 内部 callsite”，不是“真实函数起点”。从这一节开始，统一改成：**真实函数起点 + 历史偏移**。
- 旧 `0x03188E88` / `0x143188E48 + 0x40` 这一组已经可以判死；它在当前 build 里不再是字幕入口。
- 当前 build 下，`Builder A (0x00350050)` 这格在新库里还没被稳定切成函数；在 `auto_analysis_ready=false` 之前，不要再把它写成已确认函数入口。

### 0C.1 历史 probe 点 vs 当前真实函数起点

| 历史点 / 旧写法 | 当前真实函数起点 | 偏移 | 当前解释 |
|---|---|---|---|
| `0x003857E0` / `sub_1403857E0` | `sub_140385720` | `+0xC0` | gameplay 字幕 payload packer / broadcaster 历史 probe 点 |
| `0x003873E0` / `sub_1403873E0` | `sub_1403872C0` | `+0x120` | `StartTalkFunction` producer 历史 probe 点 |
| `0x00350460` / `sub_140350460` | `sub_140350380` | `+0xE0` | Builder B (`MsgStartTalk`) 的历史稳定分类点 |
| `0x00350960` / `sub_140350960` | `sub_140350790` | `+0x1D0` | Builder C (`MsgDSStartTalk`) 历史点 |
| `0x00350C70` / `sub_140350C70` | `sub_140350790` | `+0x4E0` | shared `StartTalk` materializer 内部偏移，不是独立函数 |
| `0x00388950` / `sub_140388950` | `sub_140388940` | `+0x10` | `StartTalkFunction` ctor 相关内部偏移 |
| `0x00388DE0` / `sub_140388DE0` | `sub_140388DC0` | `+0x20` | `NotificationQueueImpl` ctor 内部偏移 |
| `0x00780710` / `sub_140780710` | `sub_140780690` | `+0x80` | 历史 UI wrapper 点；当前不应再直接当 `ShowSubtitle` 真入口 |
| `0x00780810` / `sub_140780810` | `sub_140780740` | `+0xD0` | 当前 UI show/remove 簇内部偏移 |
| `0x0078A040` / `sub_14078A040` | `sub_140789C80` | `+0x3C0` | UI message queue 内部偏移 |
| `0x00C73DF0` / `sub_140C73DF0` | `sub_140C73BF0` | `+0x200` | `PlayVoiceByControllerDelay` 历史点 |
| `0x00DAC1B0` / `sub_140DAC1B0` | `sub_140DAA410` | `+0x1DA0` | `DollmanVoiceDispatcher` 历史点 |
| `0x00DAFAD0` / `sub_140DAFAD0` | `sub_140DAF8A0` | `+0x230` | `SelectorDispatch` 历史点 |
| `0x00F04620` / `sub_140F04620` | `sub_140F04490` | `+0x190` | `hash -> family` 上游入口历史点 |

### 0C.2 当前 UI 簇的更可信写法

- `sub_140780740` 当前更像真正构造 `MsgShowSubtitle` 并把消息送进 UI queue 的 sender。
- `sub_140780840` 和它形成配对，当前更像 `RemoveSubtitle` sender。
- `sub_140789C80` 是当前 build 里更稳定的 UI message queue 函数；`0x14078A040` / `0x14078A070` 更像它内部 callsite。
- 因此后续谈“字幕尾部”时，优先写：
  `sub_140780740 -> sub_140789C80`
  而不是继续把 `0x00780710` 当成 `ShowSubtitle` 真入口。
- 旧文档里仍然出现的 `sub_140780710 / sub_14078A040`，默认都按“历史 probe 偏移”理解。

## 0. TL;DR

- **音频静音**: Wwise `PostEventID` 黑名单拦截 (legacy 路径, ini 开关可关)
- **字幕 UI 咽喉**: 当前更可信的 sender 是 `sub_140780740`，旧 `sub_140780710` 现在按历史 wrapper 偏移理解
- **字幕上游 producer**: 历史点 `sub_1403873E0` 实际位于 `sub_1403872C0 + 0x120`
- **gameplay family 链路修正**: 历史点 `sub_140F04620` 实际位于 `sub_140F04490 + 0x190`；`sub_140F000B0` 才是 `family -> selector/control-state` 主解释器；`sub_140F01F90` 是 timer/pending 推进层；`sub_140F028D0` 是晚期 special gate，不是根轴
- **mainline owning component 新坐实**: `sub_140F04620 / sub_140F03AB0 / sub_140F000B0 / sub_140F03180 / sub_140F295D0` 这一整簇已经不是匿名 gameplay 黑箱；它们静态上属于 `DSElevenMonthBBControllerComponent`。`sub_140EFFEE0` 会注册 `EDSElevenMonthBBReactionEvent`，字段名直接露出 `TargetEvent / ForceToOverride / loop_time`；`sub_140F2B0A0` 还把 `RUNNING_REACTION_EVENT_{ST,LP,ED}` 和一整组 `EVENT_BB_*` 反应名写进对象。**这说明当前主链已经摸到 component/resource 语义层**，不是只停留在 selector case。命名目前仍是 BB/legacy 风格，和 live Dollman 行为之间需要继续做谨慎映射，不能仅凭类名直接下结论。
- **shared assembly 证明修正**: `sub_140350460` 已从“表层最强候选”升级成 B / `MsgStartTalk` 路首个稳定分类点；它在进 `sub_140350C70` 前已同时落成 `source-group/list`、active source、group descriptor、sound-instance wrapper family；`sub_140356F30` 仍是 `sub_140350C70` 后面的 async-only serializer
- **PVCD / instance 证明修正**: `sub_140C78F20` 不是纯 Dollman ctor，而是先写 `DSRadioSentenceGroupThroughEntityInstance` 基形，再覆写成 `DSRadioSentenceGroupThroughDollmanInstance`；`sub_140C730B0` 是 `Entity / Player / Dollman` 共用的 secondary-vfptr 方法，不是 family-specific ctor
- **Dollman-only 子系统边界坐实**: `sub_140B0DB60 / sub_140B0CCD0 / sub_141EBEA60 / sub_141EBBFC0 / sub_140FF6B60` 证明 `DSDollmanTalkManager` 更像 `story-demo + wakeup + private-room bridge + lifecycle` 混合系统，当前不像 gameplay chatter 主链
- **旁路线修正**: `sub_140470040` (`SpeakEventRep`) 和 `sub_140C79A90` (`DSRadioSentenceGroups` 新 caller 簇) 值得升优先级；`0x140B5BC70 / 0x140B5CC30` 已收敛成 `DSRadioEpilogue/PrologueEventInstance::StartState`
- **降级结论**: `sub_140388950 / NotificationQueueImpl / SubtitlesProxy / ShowSubtitle` 仍有观测价值,但不再是第一批突破点
- **v3.17 关键修正**: 历史点 `sub_1403857E0` 实际位于 `sub_140385720 + 0xC0`；它不是“统一 talk dispatcher”，而是 **gameplay-only 的字幕 payload packer / broadcaster**。实机日志已对齐出 `a1[1] == ShowSubtitle.p7 (speaker LocalizedTextResource*)`、`a1[2] == ShowSubtitle.p6 (text LocalizedTextResource*)`；因此这里已经在**字幕侧**，不再是音频/字幕统一拦截点
- **当前精准字幕规则**: 当前 live 已重新坐实 gameplay Dollman 的 sender pair = `(speaker_tag=0x12B6F, caller_rva=0x385C1B)`；旧 `0x38598B` 规则降格成历史结论
- **gameplay direct emit 显式 selector 已收敛成 5 个**: `0x11 / 0x12 / 0x17 / 0x14 / 0x05`
- **throw/recall 实测走 C (MsgDSStartTalk)**(v3.15 实机修正,之前推测 B 错误)
- **分类机制修正**: producer 层 deref `*(*(this+200)+72)` 读 `words[1] >> 32` 当 hi32 tag; 它更像 producer / SoundHelper 摘要,不是 gameplay 架构根判别轴
- **新静态结论**: `sub_140F0DCA0` 是 gameplay talk state 对象的唯一 ctor/init 点(当前只见 1 个 caller = `sub_140E31AD8` @ `0x140E31AF1`), `+0x660 / +0x6B0 / +0x700` 不是外部资源表,而是对象内 inline control window
- **live 进程修正**: 当前找到的 `0x2e5e6283000` 是 `DSCameraMode` 基类实例,但 `0x2e5d3213000` 不是 camera owner,而是 `DSPlayerState`; `DSCameraMode` 自身同时回链到 `DSThirdPersonPlayerCameraComponent` / `CameraEntity` / `DSPlayerEntity`
- **ShowSubtitle payload 当前结论**: 当前 build 的 live sender 路径已经重新恢复 `p6/p7` 直读能力；修正后的 `LocalizedTextResource` vtable RVA 是 `0x3448D38`，`+0x20` 仍是文本指针，`+0x28` 仍是长度
- **当前可直接读出的 live 样本**: `p6 tag 0x1F4 -> "My time to shine." / "How'd I do, Sam?"`, `p7 tag 0x12B6F -> "Dollman"`, `p7 tag 0x122A8 -> "Sam"`, `p6 tag 0x222C -> "Take care not to attract the enemy's attention."`, `p6 tag 0x4045 -> 任务失败文案族`
- **marker 路径修正**: `0x4AB17768 / 0x3126CDE8` 的 live 命中周围是 `ShadingGroup / VertexArrayResource / SkeletonAnimationResource / ModelPartResource`, 更像模型/骨骼/marker 资源簇,不是直接的 talk family key
- **当前 hotkey**: J/K/N/M 只是运行时手工 mute 分组,不等于最终 family 命名; 当前已确认 **J 覆盖 throw/recall/换帽/换眼镜/野外休息**, **N 至少覆盖拿出狙击枪**, **F8 session 分隔, F9 清空 log**, F12 全清 mute
- **非目标 / 架构外未覆盖**: 休息室剧情对话 (`DSTalkManager` 独立通路)、任务失败字幕 (死亡 category 路径)
- **迭代效率**: v3.15 proxy 文件触发 + `build.ps1` 自动 unload/copy/load,不再需要按 F10

## 1. 当前已安装的 Hook

> 本表 `入口` 一列默认记的是历史 probe RVA，不等于当前新 IDA 里的真实函数起点；真实起点以 `0C.1` 为准。

| 入口 | 函数 | 作用 | 装载条件 |
|---|---|---|---|
| `PostEventID` export | Wwise 音频事件入口 | eventId 黑名单 (legacy 音频) | ini `EnableDollmanRadioMute=1` |
| `0x00C73DF0` | `sub_140C73BF0 + 0x200` (`PlayVoiceByControllerDelay` 历史点) | gun/hat/glasses/chatter 音频 | ini `EnableDollmanRadioMute=1` |
| `0x00DAA410` | `sub_140DAA410` (`DollmanVoiceDispatcher` 当前真实入口) | 内层 dollman 语音分发 | ini `EnableDollmanRadioMute=1` |
| `0x00780690` | `sub_140780690` (`SubtitleRuntime` wrapper) | 当前 build 的 UI runtime 侧字幕观测/静音入口之一 | ini `EnableSubtitleRuntimeHooks=1` |
| `0x00780740` | `sub_140780740` (`ShowSubtitle` sender) | 当前 build 的真 `ShowSubtitle` sender；active mute 目前主要落在这里 | ini `EnableSubtitleRuntimeHooks=1` |
| `0x00780840` | `sub_140780840` (`RemoveSubtitle` sender) | paired remove/hide sender；用于对齐 sender `q2/q3` 与 remove `key0/key1` | ini `EnableSubtitleRuntimeHooks=1` |
| `0x003872C0` | `sub_1403872C0 + 0x120` (`StartTalkFunction` producer 历史点) | 分类 speaker → 设 TLS family flag；当前默认关闭，只作离线研究 | ini `EnableSubtitleProducerProbe=1` |
| `0x00350050` | Builder A 历史点；当前新 IDA 尚未稳定切出真实函数边界 | probe log + caller RA + msg payload dump | ini `EnableBuilderProbe=1` |
| `0x00350460` | `sub_140350380 + 0xE0` (`MsgStartTalk` 历史点) | probe log + caller RA + msg payload dump | ini `EnableBuilderProbe=1` |
| `0x00350960` | `sub_140350790 + 0x1D0` (`MsgDSStartTalk` 历史点) | probe log + caller RA + msg payload dump | ini `EnableBuilderProbe=1` |
| `0x00B5BC70` | Builder U1 = 0x388950 未定类 caller | probe log (候选 gameplay builder) | ini `EnableBuilderProbe=1` |
| `0x00B5CC30` | Builder U2 = 0x388950 未定类 caller | probe log (候选 gameplay builder) | ini `EnableBuilderProbe=1` |

**legacy 音频黑名单 eventId**:
```
2820786646 throw | 2978848044 recall | 2995625663 equip
1966841225 random chatter | 302733266 task-failure | 448888368 fall chatter
```

## 2. 关键静态反编译发现

### 2.1 字幕 UI 下游 (`sub_140780710`)

函数体内构造 `MsgShowSubtitle::vftable` → `sub_14078A040(manager, ...)` 分发。通过 `GameViewGame` vtable (`0x143188e48`) slot +0x40 间接调用, `caller_count=0`。

`_RDX` 结构 (v3.4 probe 实测):

| offset | 内容 |
|---|---|
| `0x00` / `0x08` | text ref 1 / text ref 2 (refcount @ -0x10) |
| `0x10..0x30` | 32B payload (q2 + q3 + q4 + q5) |
| `0x30` / `0x38` | q6 / q7 (heap ptr) |
| `0x40..0x4B` | 3 个 float (duration 相关) |
| `0x4C` / `0x4D` | byte flag |

### 2.2 chatter vs 死亡字幕 payload 对比

| 字段 | chatter (`caller=0x388037`, dollman/Sam/NPC 共用) | 死亡/任务失败 (`caller=0x202eefe`) |
|---|---|---|
| q2 / q3 | **恒定** `0xD4A974A2BF71653 / 0x836B97A035CBB78D` (通用 wrapper sentinel) | **每次不同** (真实文本 hash) |
| q4 / q5 | 恒定 `0 / 0` | **恒定** `0xf440886f473f32f5 / 0xb784451511dd7fa0` (死亡 category) |
| q7 | 3 值变化 (疑 speaker ptr) | 恒定 (speaker ptr) |

**结论**: q2/q3 是 DSTalkInternal chatter 路径通用 sentinel,**不是** dollman 身份 ID。v3.6 按 q2/q3 过滤 → 灭全部对话字幕 (误伤 Sam + NPC), 由此判定下游 payload 不足以区分 speaker, 必须上游 producer 层做身份识别 (驱动 v3.8+ identity probe)。

### 2.3 Dollman 相关 RTTI 类名 (字符串池可见)

- `DSRadioSentenceGroupThrough{Dollman,Entity,Player,Speaker}Instance` — 4 张平行 vtable
- `DSDollmanTalkManager` / `EDSDollmanTalkState`
- `DSTalkInternal::SubtitlesProxy` / `DSTalkInternal::NotificationQueueImpl`
- RTTI type_descriptor: `StartTalk@DSTalkManager@@`, `StartTalkFunction@DSTalkInternal@@`

### 2.4 Instance 类谱系 (4 张平行 vtable)

| RTTI | vtable | PVCD slot | RVA | 大小 | 状态 |
|---|---|---|---|---|---|
| Dollman | `0x143208A18` | slot 8 | `0xC73DF0` | 174 B | ✅ v2.2 已 hook |
| Player | `0x1432089A8` | slot 8 | `0xC73970` | 174 B | ❌ v2.5 实测 0 hits |
| Speaker | `0x1432082C8` | **slot 2** | `0xC741E0` | **828 B** | ⭐ 未测 (内部构造 `NotificationQueueImpl`) |
| Entity | `0x1432083D8` | — | — | abstract (`_purecall`) | 纯虚 |

**v3.18 静态扩图**:
- `speaker` 线已从“只知 slot2”升级为“静态已坐实一簇”: `sub_140C790A0` (主 ctor/factory) → `sub_140C741E0` (secondary-vfptr init/dispatch) → `sub_140C73FF0` (cleanup)
- `entity / player / dollman` 共用 `sub_140C730B0` 当 secondary-vfptr init/dispatch；`sub_140C78DE0` = `Player` ctor, `sub_140C78F20` = `Dollman` ctor
- 当前表层还没看到一个和 `speaker/player/dollman` 对称的“独立 entity thin ctor”；更像 `entity` 是 base shape,`player/dollman` 在其上切 vfptr

### 2.5 音频调用链 (throw/recall)

```
Wwise::PostEvent → 0x26B5EF0 (wrapper) → 0x29B270 (game API) → 0x28F780 (emitter tick)
```

音频是 **tick 驱动**的 emitter 状态机, 字幕与音频 **并行独立通路** → 不能从音频 hook 抓字幕。throw/recall 还走 Wwise `external source` (`externalSources=1`, 磁盘直读), 和普通 chatter 音频路径也不同。

### 2.6 gameplay 主链钉死 (v3.14 静态 + v3.15 实测)

**新增收敛结论**:

- gameplay direct emit **不是开放集合**。入口 event hash 先进入 `sub_140F04620` 写 pending / timer, 再由 `sub_140F01F90` + `sub_140F000B0` 调度, 最终只落到 `sub_140DAFAD0(a1, selector)` 的显式 selector / case6~8 control window。
- `sub_140DAFAD0` 的 `a2` 是 **sentence-group selector**,不是动作 hash。它从 `a1[6] + 40` 的 group 表里按 selector 取组,组内随机选一句、避免和上一句重复,构造 `MsgDSStartTalk`,最后交给 `sub_140160ED0`。
- 目前 direct gameplay 已确认会直接 emit 的 **显式 selector** 是 **`0x11 / 0x12 / 0x17 / 0x14 / 0x05`**；其中 `0x11 / 0x12` 是同一分支按 `[this+0x524]` 分叉。

**入口 event hash 只负责 pending/state,不是最终 family**:

| event hash | 当前结论 |
|---|---|
| `0x5A8DAE68` | pending 0 (最终 likely 落 `slot 0x12`) |
| `0x4AFE7080` | pending 2 (最终 likely 落 `slot 0x17 / 0x14`) |
| `0x2FBDBEC3` | pending 7 (最终 likely 落 `slot 0x05`) |
| `0x27287CA6` | `pending = *(a2+24)`,要求 `< 0x14` |
| `0x33D1F00A` | pending 1 + timer |
| `0x66B348E3` | pending 1 变体 |
| `0x7D234852` | pending 5 变体 |
| `0x4C8EA496` | pending 6 |
| `0x5DE47D18` | reset / resync (`sub_140F030A0` + `sub_140F02880(0)`) |
| `0x6108DFB1` | 只更新随机参数窗口 (`+558/+564`) |
| `0x108FE50B` | 特殊 marker 路径,配合 `0x4AB17768` / `0x3126CDE8` |

**整条链 (从触发到 UI)**:

```
gameplay action
  → sub_140F04620 (event hash 入口 / 写 pending)
  → sub_140F01F90 (pending/state 调度)
  → sub_140F000B0 (9-state emitter / case 0..8)
  → sub_140DAFAD0 (按 sentence-group selector 组装 MsgDSStartTalk; 显式 emit = 0x11 / 0x12 / 0x17 / 0x14 / 0x05)
  → sub_140160ED0 (EntityMessaging dispatcher, 1991 callers, 通用非专属)
     └─ binary-search msg_type_hash in type_desc array
     └─ call [slot+8] (indirect, handler thunk)
  → 0x140354AB0/AC0/AD0 thunk (jmp only)
  → sub_140350050/460/960 = A/B/C (DSOnTileEntityVariableComponentResource handlers)
     ├─ A builder = MsgExpressSignal (lightweight signal/reaction)
     ├─ B builder = MsgStartTalk (general chatter)
     └─ C builder = MsgDSStartTalk (heavy, 带 sequence/lipsync/facial)
  → sub_140388de0 (构造 DSTalkInternal::NotificationQueueImpl, 3 个 direct caller 刚好 = A/B/C)
  → sub_1403896f0 (构造 Simple/DummySoundInstanceWrapper,builder B/C + 其他 5 处也用)
  → sub_140388950 (构造 DSTalkInternal::StartTalkFunction,+0x88 = SoundHelper+0x10 secondary)
  → sub_1403541c0 (DSTalkComponentRep::SoundHelper ctor,primary vtbl @0x143132678 + secondary @0x143132688)
  → StartTalkFunction::update (sub_1403873E0, producer, slot 15)
     └─ sub_140387030 拉 Entry28 → sub_14034FC30 (secondary slot1, 纯 SIMD copy,无分流)
     └─ sub_140387f20 (条件检查)
  → sub_1403857E0 (gameplay-only 字幕 payload packer; `a1[1]=speaker`, `a1[2]=text`)
  → sub_140780710 (GameViewGame::ShowSubtitle,字幕 UI 咽喉)
```

**handler table 布局** (@ `0x1441fa7c0`,共 10 条,16B/条 `{msg_desc_ptr, handler_thunk}`):

| Slot VA | msg_desc | thunk | thunk 指向 | builder ID |
|---|---|---|---|---|
| `0x1441fa7c0` | `0x1441C81E0` | `0x140354CC0` | sub_140354CC0 | — |
| `0x1441fa7d0` | `0x1441D3150` | `0x140354C60` | sub_140354C60 | — |
| `0x1441fa7e0` | `0x1441D04D0` | `0x140354BD0` | sub_140354BD0 | — |
| `0x1441fa7f0` | `0x1441D1C00` | `0x140354B20` | sub_140354B20 | — |
| `0x1441fa800` | `0x14423EA20` | `0x140354AE0` | sub_140354AE0 | — |
| `0x1441fa810` | `0x1441D3200` | `0x140354AD0` | **sub_140350050** | **A** (MsgExpressSignal) |
| `0x1441fa820` | `0x1441D1400` | `0x140354AC0` | **sub_140350460** | **B** (MsgStartTalk) |
| `0x1441fa830` | `0x1441FAC90` | `0x140354AB0` | **sub_140350960** | **C** (MsgDSStartTalk) |
| `0x1441fa840` | `0x1441D15C0` | `0x1403549C0` | sub_1403549C0 | — |
| `0x1441fa850` | `0x1441C7FD0` | `0x140354960` | sub_140354960 | — |

**相关字符串** (无 xref,Decima hash reflection):
- `0x1441d0084` = "MsgExpressSignal"
- `0x1441d124c` = "MsgStartTalk"
- `0x1441fa15c` = "MsgDSStartTalk"
- `0x1441fa75c` = "DSOnTileEntityVariableComponentResource" (表头前 0xB4 字节)

**dispatcher 关键指令** (`sub_140160ED0`):

```asm
14016107c  mov rdx, r15              ; msg obj 作第二参数
14016107f  call qword ptr [rax+8]    ; handler (rax = slot+0 = subscriber | flag_LSB)
140161082  mov eax, [rsi]            ; builder hook 里 caller_rva 落点
```

handler 调用形式: `handler(subscriber_this, msg_obj)`,this 来自 `slot[0] & ~1`(LSB 位清标志),msg_obj 在栈上。

**v3.15 实测修正**:

v3.12 era 把 J 组 (hi32 = 0x1f4) 的 throw/recall 归到 builder B 是**错**的。实机 throw/recall 6 次全部命中 builder C (MsgDSStartTalk),无一命中 A/B。所以 **throw/recall 走 C = 重型 DSStartTalk 路径**,和 "C = 带 sequence/lipsync/facial 资源" 的静态画像吻合。

**关键推论**:
- hi32 不是 speaker ID,是 **SoundHelper 缓存摘要** — 因为 identity (`*(*(this+200)+72)` 的 words) 在 0x3541c0 构造 SoundHelper 时**从上游灌进来**,不是 StartTalkFunction 自己算的。同一 speaker 在不同场景 (不同 entity variable link / resource row) 得到不同 hi32 tag。
- **gameplay 未发现域** 更像是 "**尚未把动作域映射到 5 个显式 selector + case6/7/8 control window / 对应 resource row**",而不是又有一套新的 direct emit 架构。
- hi32 (`0x1f4 / 0x222c / 0x4377 / 0x3745 ...`) 适合做运行时观测 / 热键分组,但真正该收敛的根轴是 **event hash → pending/state → 5 个显式 selector + case6/7/8**。

### 2.6.1 `sub_140F000B0` = 9-case jump table

`sub_140F000B0` 在 `0x140F0031C` 通过 jump table 按 `current_state = byte[this+0x520]` 分到 9 个 case:

| state | case RVA |
|---|---|
| 0 | `0xF00328` |
| 1 | `0xF006DD` |
| 2 | `0xF007EE` |
| 3 | `0xF0139A` |
| 4 | `0xF014B7` |
| 5 | `0xF01658` |
| 6 | `0xF01B8B` |
| 7 | `0xF01C5B` |
| 8 | `0xF01DC4` |

显式 selector 的关键 callsite:

- `0x140F00574` → `mov dl, 0x11`
- `0x140F0057D` → `mov dl, 0x12`
- `0x140F00AB4` / `0x140F00F97` / `0x140F0190A` → `mov dl, 0x17`
- `0x140F011C6` → `mov dl, 0x14`
- `0x140F01CF1` / `0x140F01D30` → `mov dl, 0x05`

case6/7/8 的 control-window 读点:

- case6: `0x140F01BF6` → `mov edx, dword ptr [rdi + rax*4 + 0x660]`
- case7: `0x140F01C76` → `mov edx, dword ptr [rdi + rax*4 + 0x6B0]`
- case8: `0x140F01DDF` → `movsxd rbx, dword ptr [rdi + rax*4 + 0x700]`

### 2.6.2 `sub_140F0DCA0` = control window 的 ctor/init 点

### 2.6.3 gameplay 主链的 owning component 已命名

之前 `sub_140F04620 -> sub_140F01F90 -> sub_140F000B0 -> sub_140DAFAD0` 这条链虽然已经被静态和 runtime 钉成主轴，但“它到底是谁的 component”还没有坐实。2026-04-25 的静态补图已经把这个问题补上了:

- `0x143242BE8` = `DSElevenMonthBBControllerComponentSymbols::vftable`
- `0x143242CF8` = `DSElevenMonthBBControllerComponent::vftable`
- `0x143248800` = `DSElevenMonthBBControllerComponentResource::vftable`
- `0x14434CEC0` 处的全局对象直接带字符串 `"DSElevenMonthBBControllerComponent"`
- `0x14434CA20` / `0x14434C780` 两张 `.data` 回调表属于同一 component，全是 `0x140F29xxx / 0x140F03xxx / 0x140F04xxx` 这一簇方法

关键命名证据:

- `sub_140EFFEE0` 注册:
  - `"DSElevenMonthBBControllerComponentSymbols"`
  - `"DSElevenMonthBBControllerComponent"`
  - `"UUIDRef_DSElevenMonthBBControllerComponent"`
  - `"EDSElevenMonthBBReactionEvent"`
  - 字段名: `"TargetEvent" / "ForceToOverride" / "loop_time"`
- `sub_140F2B0A0` 初始化:
  - `ELEVEN_MONTH_BB_STATE_RUNNING_REACTION_EVENT_ST`
  - `ELEVEN_MONTH_BB_STATE_RUNNING_REACTION_EVENT_LP`
  - `ELEVEN_MONTH_BB_STATE_RUNNING_REACTION_EVENT_ED`
  - 以及大批 `EVENT_BB_*` / facial animation 名

对主链的直接含义:

- `sub_140F04620` 不再只是“匿名 event-hash 入口”，而是这个 component 的 reaction-event 归一化/分发层之一。
- `sub_140F02DB0` 已露出更高一层的 reaction 写入语义: `< 0x14` 的 reaction slot 竞争、`ForceToOverride`、`loop_time` 等参数，说明它在 `family/selector` 之上。
- `sub_140F03180` / `sub_140F295D0` 会围绕同一 component 的对象字段 `+0x4xx ~ +0x7xx` 初始化/刷新运行态，并把结果继续推到后面的 selector / talk emit 主链。

当前谨慎结论:

- **我们已经摸到这条 gameplay chatter 主链的 component/resource 顶层命名边界**。
- 但这个边界的类名仍然是 `DSElevenMonthBBControllerComponent` / `EDSElevenMonthBBReactionEvent`，呈现明显 BB / legacy 命名风格。
- 因此后续 agent 不能简单把“类名里没写 Dollman”当成排除依据；需要继续把这个 component 的 live 行为和当前游戏里的 Dollman gameplay chatter 做一一对齐，再决定最终拦截点。

`sub_140F0DCA0` 当前只看到 **1 个 caller**:

- `sub_140E31AD8` @ `0x140E31AF1`

这个 ctor/init 直接在对象里铺出 `+0x600 / +0x640 / +0x680 / +0x6C0` 四个重复 control record,然后继续填 `+0x700..+0x768` 的 helper/handle qword。关键事实:

- `+0x660 / +0x6B0 / +0x700` **不是外部 data blob**
- 它们是对象内部的 **inline control window**
- `+0x700` 这一窗后半段不是纯字面常量,会在 ctor 后半段被 helper/handle qword 覆盖

当前能直接从 ctor 看到的硬编码初始化:

- `0x140F0DF2F` → `[this+0x664] = 0x3DCCCCCD`
- `0x140F0DF39 / 0x140F0DF43` → `[this+0x668/0x66C] = 0x477FE000`
- `0x140F0DF7D / 0x140F0DF87` → `[this+0x6A0/0x6A4] = 0x3F800000`
- `0x140F0DF91 / 0x140F0DF9B` → `[this+0x6A8] = 0x44000000`, `[this+0x6AC] = -1`
- `0x140F0DFA5 / 0x140F0DFF4` → `[this+0x6B0]` 与 `[this+0x6F0]` 都写入 `0x428C0000`
- `0x140F0E014` → `[this+0x700] = 0`
- `0x140F0FEDA / 0x140F0FC01 / ... / 0x140F0F7BB` → `+0x708 / +0x710 / ... / +0x748` 后续被写成 helper/handle qword

**结论修正**: “三张表初始化来源未明 / 可能 data-driven” 这个判断可以下线。初始化来源已经坐实在 `sub_140F0DCA0`；真正剩下的是 **哪些 pending 会实际命中这些 control window 的哪些 cell**。

### 2.6.3 live 对象关系修正 (`DSCameraMode` 不是孤立 talk 类)

当前 live 进程 (PID `32660`) 已直接扫到一个 `DSCameraMode` 基类实例:

- `DSCameraMode` base object = `0x2e5e6283000`
- 主 vtable = `image_base + 0x32430B8`
- 次基类 vtable = `image_base + 0x3242D68`
- `[base+0x600/+0x640]` = `CameraDoFProperties`
- `[base+0x680/+0x6C0]` = `CameraMiscProperties`

**关键修正**: 之前怀疑的 `0x2e5d3213000` 不是 “camera owner”,而是 RTTI=`DSPlayerState`:

- `0x2e5d3213000` → vtable `image_base + 0x3230908` → `.?AVDSPlayerState@@`

而 `DSCameraMode` 自身能直接回链到:

| 偏移 | live 指针 | RTTI |
|---|---|---|
| `[base+0x2E28]` | `0x2e5af401c80` | `DSThirdPersonPlayerCameraComponent` |
| `[base+0x30]` | `0x2e5ff7ff600` | `CameraEntity` |
| `[base+0x540]` | `0x2e612530020` | `DSPlayerEntity` (subobject) |

**含义**:
- `sub_140F000B0 / sub_140F01F90 / sub_140F04620 / sub_140F10DC0` 这一整簇确实在 **player / camera runtime** 里
- 但不能再把它粗暴叫成“纯 talk 状态机”
- 更准确的说法是: **gameplay camera/player state 里夹带了 dollman / gameplay 字幕触发**

### 2.6.4 `sub_140F10DC0` 的 7 个 hash 已被 live 命名

`sub_140F10DC0` 从 `[base+0x700]` 取 resource, 再走 `resource + 0x3F8` 的 hash→value 表。当前 live 解析结果:

| hash | name |
|---|---|
| `0x4186BC17` | `JNT_C_B_000_Root` |
| `0x53364FD4` | `JNT_C_B_001_Hips` |
| `0x127C6C7F` | `JNT_C_B_008_Head` |
| `0x7AB42FBC` | `HLP_Odradek` |
| `0x65403C9F` | `MTP_Global_a` |
| `0x7610CF6B` | `MTP_Global_b` |
| `0x047B4C68` | `MTP_Global_c` |

对应 value object 的 RTTI = `JointID`。

**结论**:
- 这 7 个 hash 是 **joint / helper / marker 名**
- 不是 throw/recall 的 talk family
- `sub_140F10DC0` 这条线更像 **camera anchor / model binding**

### 2.6.5 历史结论: ShowSubtitle payload 的 `p6/p7` = `LocalizedTextResource`

旧 live 会话曾把 `[show]` 里的 `p6/p7` 直接坐实为:

- vtable RVA = `0x3448E48`
- RTTI = `.?AVLocalizedTextResource@@`
- `+0x20` = 文本指针
- `+0x28` = 文本长度

**已直接读出的 live 样本**:

| 来源 | tag | 直接文本 |
|---|---|---|
| `p6` (当前 throw/recall 样本) | `0x01F4` | `"My time to shine."`, `"How'd I do, Sam?"` |
| `p7` | `0x12B6F` | `"Dollman"` |
| `p7` | `0x122A8` | `"Sam"` |
| heap 扫描残留样本 | `0x222C` | `"Take care not to attract the enemy's attention."` |
| heap 扫描残留样本 | `0x4045` | 任务失败文案族 (`"This order's a failure."`, `"You can't complete the order."` 等) |

**这条发现很值钱**:
- runtime 已经可以不靠猜测,**直接读到字幕正文 / speaker 名**
- 后续只要拿到 `[show]` 的 `p6/p7` 指针,就能把 gameplay tag 和真实台词对上
- 这比继续在 hi32/tag 上空转更有效

**但要加一条当前 build 的保留意见**:
- 2026-04-26 当前 live sender 命中日志里，`p6_ok=0 / p7_ok=0`，所以这组直读能力必须重新验证后才能继续当主依据。

### 2.6.6 `0x108FE50B / 0x4AB17768 / 0x3126CDE8` 更像 marker / model 资源簇

对 live 命中做内存邻域检查:

- `0x4AB17768` / `0x3126CDE8` 命中周围同时出现的 RTTI / vtable:
  - `ShadingGroup`
  - `VertexArrayResource`
  - `SkeletonAnimationResource`
  - `ModelPartResource`

**结论修正**:
- `0x108FE50B` 相关 “special marker path” 更像模型/骨骼/marker 资源分支
- 这条线可能仍参与 camera / animation 同步
- 但它 **不像** 能直接拿来当 gameplay 字幕 family key

### 2.7 sound wrapper + notification queue 内部类

| RTTI | vtable | 分配点 | 用途 |
|---|---|---|---|
| `DSTalkInternal::SimpleSoundInstanceWrapper` | `0x143135548` | sub_1403896f0 | 带 vtable 的 sound instance |
| `DSTalkInternal::DummySoundInstanceWrapper` | `0x1431354B8` | sub_1403896f0 | 空 wrapper |
| `DSTalkInternal::NotificationQueueImpl` (primary) | `0x1431358E0` | sub_140388de0 | msg notification queue |
| `DSTalkInternal::NotificationQueueImpl` (secondary, +0x10) | `0x143135368` | sub_140388de0 | 双基类第二 base |
| `DSTalkInternal::StartTalkFunction` | `0x143135930` | sub_140388950 | talk state machine |
| `DSTalkComponentRep::SoundHelper` (primary) | `0x143132678` | sub_1403541c0 | talk provider |
| `DSTalkComponentRep::SoundHelper` (secondary, +0x10) | `0x143132688` | sub_1403541c0 | slot1 = sub_14034FC30 (copy only) |

**`sub_1403896f0` 所有 caller (7 个)** — 说明 wrapper 是通用工具:
- 0x350770 (builder B), 0x350afc (builder C)
- 0x387d95 (StartTalkFunction 方法簇自身)
- 0x470288 (NPC 通路 sub_140470040,也是 0x388950 的 caller 之一)
- 0x140b5beb8 / 0x140b5cd45 (分别位于 `DSRadioEpilogue/PrologueEventInstance::StartState` 内)
- 0x140c794d9 (sub_140C79480 新通路,不在 0x388950 列表,低优先级)

**`sub_140388de0` 直接 caller 恰好 3 个** = A/B/C → 表明 NotificationQueueImpl 构造是 gameplay 三 builder 的专有事。

**`sub_140388950` 直接 caller 5 个**:
- 0x3533ae (in sub_140353060, gameplay 中转,0x353060 只被 sub_140350C70 gateway 调用)
- 0x470301 (NPC 通路)
- 0x140b5bf31 (`DSRadioEpilogueEventInstance::StartState`)
- 0x140b5cdb6 (`DSRadioPrologueEventInstance::StartState`)
- 0xc7293a (DSRadio / dollman random)

### 2.8 v3.18 大范围表层扫图新增收敛

#### 2.8.1 gameplay builder / shared assembly 新表层突破点

这一轮最大的优先级修正:

- `sub_140350460` 是当前最强的 gameplay builder 切点。它在进 `sub_140350C70` 前，已经选定 `speaker/source` 相关对象，并用 `sub_1403896F0` 做 wrapper family 分类。
- `sub_140350960` 是重型 `MsgDSStartTalk` builder；当前 throw/recall 的已知实 traffic 明确会走这条 C 路。
- `sub_140350050` 是最轻的 `MsgExpressSignal` builder，适合以后做“极窄验证切点”。
- `sub_140350C70` 仍是 A/B/C 的唯一 shared sink，但更准确的定义是 **第一处统一归一化边界**，不是“第一次决定 speaker 身份”的点。
- `sub_140356F30` 是 `sub_140350C70` 后面的 async-only serializer；它把已组好的 `StartSentencesArgs` 风格 payload 序列化到 `EventTypeMarshal` 分支。
- `sub_140353060 -> sub_1403541C0` 是 shared sink 后最窄的 shared assembly 内层；这里首次把 helper / owner / descriptor 稳定绑成同一组对象状态。

当前可收成的表层链:

```text
0x140350050 / 0x140350460 / 0x140350960
  -> 0x140350C70
  -> (0x140356F30 async marshal | 直接 0x140353060)
  -> 0x1403541C0
  -> 0x140388950
```

#### 2.8.2 PVCD / instance / DSRadioSentenceGroups 层

这一层当前比 `NotificationQueueImpl / SubtitlesProxy` 更值得打。

- `sub_140C730B0` = `Entity / Player / Dollman` 共享 secondary-vfptr init/dispatch
- `sub_140C741E0` = `Speaker` 专线 secondary-vfptr init/dispatch
- `sub_140C724E0` = `DSRadioSentenceGroupsInstance` 更高一层入口 / 枢纽
- `sub_140C72200` = `speaker + entity/player/dollman` 共享 gating/eligibility
- `sub_140C790A0 / sub_140C78DE0 / sub_140C78F20` = `Speaker / Player / Dollman` 的主 ctor/factory
- `sub_140388E60 / sub_140388F30` 已经明确属于 queue action layer,不再当“首次分流点”

这轮表层骨架:

- `speaker` 是单独一族
- `entity / player / dollman` 是同一族；`player` 和 `dollman` 都像是先落到 `entity` base,再切到各自 vfptr
- `NotificationQueueImpl` 相关 helper 已经是分流之后的队列动作层

#### 2.8.3 gameplay state / event hash / selector / control-window 层

这轮把 `sub_140DAFAD0` 从“根轴”降到“selector bridge”；更像 gameplay family 真正根轴的是 `sub_140F000B0`。

当前表层主线:

```text
0x140F04620
  -> 0x140F000B0
  -> 0x140F01F90
  -> 0x140F028D0
  -> 0x140F02880 / 0x140F02F40
  -> 0x140E31A20
  -> 0x140F0DCA0
```

分层:

- `hash/pending` 层: `sub_140F04620`, `sub_140F000B0`, `sub_140F01F90`
- `selector/control` 层: `sub_140F028D0`, `sub_140F02880`, `sub_140F02F40`, `sub_140E31A20`, `sub_140F0DCA0`

这一轮新露出来、值得继续追的表层边界:

- event hash: `0x4C8EA496`, `0x108FE50B`, `0x27287CA6`, `0x2FBDBEC3`, `0x33D1F00A`, `0x4AFE7080`, `0x5A8DAE68`, `0x5DE47D18`, `0x6108DFB1`, `0x66B348E3`, `0x7D234852`
- state/mode: `0`, `1`, `3`, `5`, `6`, `7`, `8`, `0x10`, `0x1A`
- control flags: `0x400`, `0x10000`, `0x20000`

#### 2.8.4 Dollman-only manager / updater / private-room bridge

`DSDollmanTalkManager` 现在已经不是 RTTI 线索，而是明确存在的独立子系统。

- `sub_140B0DB60` = 最硬的 Dollman-only 语义注册点；直接注册 `DSDollmanTalkWakeup`、`IsRemainSelectableDollmanTalkStoryDemo`
- `sub_140B0CCD0` = `DSDollmanTalkManager::Impl` 真 ctor；初始化状态机并把全局单例写到 `qword_146230ED8`
- `sub_140B0DA40` = manager 内部条目 / node 列表筛选点，像“还有没有可选 Dollman talk / story-demo”
- `sub_140B0CE50` = 当前 state 的薄分发器
- `sub_140B0DA00` = manager 唤醒 / 置位点
- `sub_141EBEA60` = `DSGMPostU_DSDollmanTalkManager` updater/callback 入口
- `sub_141EBBFC0` = 强全局实例消费点，直接吃 `qword_146230ED8`
- `sub_140FF6B60` = private-room 和 Dollman manager 的关键桥；会直接检查当前 manager state 是否为 talking

当前最干净的 manager 启动链:

```text
0x140B0CCD0 <- 0x141EB8E70 <- 0x1407050E0
```

#### 2.8.5 旁路线 / SpeakEventRep / 重新归类的旧点

这轮“旁路线”里，真正被低估的不是 `sub_140388950`，而是 `DSRadioSentenceGroups / Through*Instance / SpeakEventRep` 这三簇更上游的方法。

- `sub_140470040` = `SpeakEventRep` 虚表方法；独立于主 gameplay builder 的真实产出路
- `sub_140C79A90` = `0x140C79480 / 0x140C79570 / 0x140C79640 / 0x140C799B0` 的 cluster head
- `sub_140C730B0 / sub_140C724E0 / sub_140C741E0` 现在应视为比 `0x140388950` 更值得优先追的旁路根点
- `0x140B5BC70 / 0x140B5CC30` 不再当“U1/U2 未定类”看待；它们已经静态收敛成 `DSRadioEpilogue/PrologueEventInstance::StartState`

### 2.9 v3.19 证明阶段新增收敛

#### 2.9.1 `0x140350460` = B / `MsgStartTalk` 首个稳定分类点

- `0x140354AC0` 只是 `jmp sub_140350460`，说明 `0x140350460` 前面没有额外分类层。
- `0x140350460` 会先把 `msg+0x10 / +0x18 / +0x20` 三路来源收敛成同一类 `source-group/list` 对象 `v13`。
- `sub_14028E9F0(v13)` 已证明是在 `v13+0x28(count) / +0x30(entry array) / +0x38(cache)` 这组字段上选出当前 active source `r14`。
- `qword_146259260(..., v13, 7)` 负责把 `v13` 变成 group-level descriptor；随后 `0x140350460` 再把 active source `r14` 塞回 descriptor。
- `msg+0x40` 直接送进 `sub_1403896F0`，被包装成 `Simple / Dummy / GraphSoundInstanceWrapper`；因此它高置信度是 concrete sound instance，而不是 source/list。
- `0x140350C70` 的 body 不再重新调用 `sub_14028E9F0 / sub_1403896F0 / qword_146259260`；它只做 payload normalize、display name 查询、extra blob 复制，再送 `0x140353060`。
- 当前最硬的结论是：对 B 路来说，`0x140350460` 已经在 shared sink 前同时固定了 `source-group/list`、active source、group descriptor、wrapper family。
- 这使它从“结构上最像”升级成“已证实的首个稳定分类点”；但静态上仍未看到显式 `if Dollman` 分支，所以它是首个稳定分类点，不是首个显式语义判定点。

#### 2.9.2 `0x140C730B0 / 0x140C78F20` = family split 层，不是 gameplay / cutscene split 层

- `0x140C78F20` 先把主 vfptr 写成 `DSRadioSentenceGroupThroughEntityInstance`，再在 `0x140C78F85 -> 0x140C78F90` 覆写成 `DSRadioSentenceGroupThroughDollmanInstance`。
- `0x140C78DE0` 也同样先走 `Entity` 基形，再覆写成 `DSRadioSentenceGroupThroughPlayerInstance`。
- `0x140C730B0` 没有普通 code xref caller；`0x1432083E8 / 0x1432089B8 / 0x143208A28` 三个 secondary-vfptr 槽都直接指向它，说明它是三家共用的 secondary-vfptr 方法。
- 真正把 `entity / player / dollman` 分开的，不是 `0x140C730B0` 本体，而是主 vfptr 和主 vtable 的唯一槽位。
- 当前最硬的 family-specific 槽位是：
- `+0x28`: `Entity -> 0x140C72090`，`Player -> 0x140C72100(写 3)`，`Dollman -> 0x140C72110(写 4)`
- `+0x40`: `Entity -> _purecall`，`Player -> 0x140C73970`，`Dollman -> 0x140C73DF0`
- `0x140C730B0` fallback 分支会执行 `mov rax, [rbx] ; call qword ptr [rax+40h]`，因此 shared secondary path 之后的 family-specific 行为最终仍由主 vtable `+0x40` 决定。
- 这一整线天然区分的是 instance family，不天然区分 gameplay / cutscene；这层没有 event hash / selector / pending-state 语义。

#### 2.9.3 `0x140F000B0 / 0x140F028D0` = gameplay family 主解释器 / 晚期 gate

- 这条线当前最关键的字段已经收敛：
- `+0x520 / +0x521` = mode / previous mode
- `+0x524 / +0x528 / +0x52C / +0x53C` = selector / previous selector / selector-bound owner / dirty companion
- `+0x534` = normalized gameplay family 根轴
- `+0x538` = family snapshot / pending-family latch
- `+0x540` = family-associated timer/value
- `+0x550 / +0x554` 与 `+0x558 / +0x564` = pending window
- `+0x590` = 全局 `0x400` snapshot
- `0x140F04620` 不是最终分流器，而是 `hash -> family(+0x534)` 的归一化入口。
- 已见映射：`0x33D1F00A -> 1`，`0x4AFE7080 -> 2`，`0x2FBDBEC3 -> 7`，`0x4C8EA496 -> 6`，`0x27287CA6 -> *(a2+0x18)`。
- `0x140F000B0` 才是 family 主解释器：它先按 `+0x520` 做 9-case switch，再大量调用 `0x140F02F40` 写 selector，并直接发 `sub_140DAFAD0(0x11 / 0x12 / 0x14 / 0x17 ...)`。
- `0x140F01F90` 是 timer/pending 推进层；它会基于时间窗继续改写 `+0x534`，例如直接写 `0x10 / 3 / 0x11 / 4`，并构造 hash `0x2FBDBEC3`。
- `0x140F028D0` 不是 family 根轴，而是晚期 special gate。它入口第一句就读 `+0x534`，核心是 selector `0x10 <-> 0x1A` 切换。
- `0x400` gate 的是全局 override/freeze 类 mode fallback，不定义 family。
- `0x10000 / 0x20000` gate 的不是另一条 family，而是强制打开 `0x10 <-> 0x1A` 这条 special control branch。
- `0x140E31A20 -> 0x140F0DCA0` 属于 control-window host/init 层，不是 family 主链根轴。

#### 2.9.4 `DSDollmanTalkManager` = 高误伤旁路，不像 gameplay chatter 主链

- `0x140B0CCD0` 是 `DSDollmanTalkManager::Impl` 真 ctor；它挂 `KJPStateMachine<Impl, EDSDollmanTalkState>` 到 `impl+48`，并写全局 holder `qword_146230ED8`。
- `0x140B0DB60` 直接注册两个明确节点：`DSDollmanTalkWakeup` 与 `IsRemainSelectableDollmanTalkStoryDemo`。
- `0x140B0DA00` 更像 wakeup / active-byte 置位点，不像 subtitle/talk payload 组包点。
- `0x140B0DA40` 更像“还有没有可选 Dollman talk / story-demo”筛选器。
- `0x140B0CE50` 是当前 state 的薄分发器。
- `0x141EBEA60` 不是 Dollman 专线；它是 `DSGameManagersUpdater` 的 post-update manager registration bucket，其中一项才是 `DSGMPostU_DSDollmanTalkManager`。
- `0x141EBBFC0` 不是 manager-core，而是全局 use-site / lifecycle sink；实 caller 已见 `OnBeforeDeInitialize / OnDeInitialized / OnUnloaded` 一类系统生命周期路径。
- `0x140FF6B60` 是 private-room bridge：先查房间/模式位，再读取 `qword_146230ED8 -> currentState`，只在 `StateTalking(+16 == 1)` 时返回真。
- 当前反证比正证更强：`qword_146230ED8` 的使用面没有碰到 `0x140350460 / 0x140350C70 / 0x140353060 / 0x140388950 / 0x140F000B0 / 0x140DAFAD0` 这一批 gameplay chatter 主链点。
- 因此这条线当前更应被定义为 `story-demo + wakeup + private-room bridge + lifecycle` 混合系统，而不是干净的 gameplay chatter 主链。

#### 2.9.5 当前我们已掌握什么 / 不掌握什么 / 下一步怎么做

**已掌握**

- `0x140350460` 已经从“候选突破点”升级成 B / `MsgStartTalk` 路首个稳定分类点。
- `0x140C78F20` 的 `entity -> dollman` 切换点已经坐实，就是主 vfptr 从 `0x1432083D8` 覆写到 `0x143208A18`。
- `0x140C730B0` 的定位已经坐实：shared secondary-vfptr 方法，不是 family-specific ctor。
- `0x140F000B0` 已经坐实为 gameplay family 主解释器；`+0x534` 是 family 真根轴。
- `0x140F028D0` 已经被降级成晚期 gate；`0x140E31A20 -> 0x140F0DCA0` 属于 control-window 宿主/初始化层。
- `DSDollmanTalkManager` 当前不像 gameplay chatter 主链，而像高误伤 Dollman-only 旁路。

**还不掌握**

- `0x140350460` 上游谁最早把 Dollman 语义装进 `msg+0x10 / +0x18 / +0x20 / +0x40`。
- `0x140350460` 的 `v13 / r14 / msg+0x40 / wrapper vtable` 在 runtime 下对 Dollman gameplay chatter 的实际值分布。
- `0x140F000B0` 各个 `case(+0x520)` 分别消费哪些 `+0x534 family`，哪一簇最稳定对齐 Dollman gameplay chatter。
- `0x140C730B0` 这条 family split 线与实际 gameplay chatter 域的 runtime 交集是否足够窄，能不能当静态+runtime 联合刀口。

**下一步**

- 第一优先级：只追 `0x140F000B0`，把 `case(+0x520) -> family(+0x534) -> selector` 全表补齐。
- 第二优先级：围绕 `0x140350460` 做 runtime probe，重点盯 `msg+0x10 / +0x18 / +0x20 / +0x40 / +0x54 / +0x5C..0x5F / +0x60+ / +0xB0`，以及 `v13 / r14 / wrapper vtable`。
- 第三优先级：把 `0x140C78F20 -> 0x140C730B0 -> [vfptr+0x40]` 这条 family split 线做 runtime 对齐，确认 Dollman gameplay chatter 是否稳定落到 `0x140C73DF0` 一簇。
- 目前不建议先从 `DSDollmanTalkManager` 线下刀；这条线的最大风险更像误伤 story-demo、private-room 和 lifecycle，而不是精准静音 gameplay chatter。

## 3. 已尝试过的字幕路径 (失败案例汇总)

| # | 路径 | 结论 |
|---|---|---|
| 1 | SubtitleParamWrite caller 分支 | 日志命中但无视觉效果 |
| 2 | Widget dt forcing (`0x1DDCC90/0x1DDCD70`) | 无效果 |
| 3 | Duration-only (key `0x3A3714ED`) | 字幕缩短但未消失 |
| 4 | Elapsed + Duration + Detach controller+0xA0 | 字幕仍显示 |
| 5 | Controller quiesce | 字幕仍显示 |
| 6 | Lifecycle route (`0xB3C39D/B3C484/B3C4D6`) | Duration 变短但未消失, RVA 疑似过期 |
| 7 | TalkSay/TalkPlaySentence thunks (`0x1F6AE0/AD0/AC0`) | throw/recall 期间不命中 |
| 8 | DollmanRadio PVCD (`0xC73DF0`, v2.2) | 覆盖普通 chatter, 不覆盖 throw/recall |
| 9 | DollmanVoiceDispatcher (`0xDAC1B0`, v2.3) | throw/recall 不命中 |
| 10 | RadioSibling PVCD (`0xC73970`, v2.5) | 0 hits, 证伪 sibling 假设 |
| 11 | ShowSubtitle + q2/q3 hash filter (v3.6) | 方向对但过滤条件过粗, 灭全部对话字幕 |

**老对象链 (历史记录, RVA 已过期)**:
slot `[rsi + rdi + 0x45F8]` → controller `[slot + 0xD8]` → paramObject `[controller + 0xA0]`

## 4. 已知陷阱 / 已排除项

### 4.1 Decima 静态 xref 断链
- RTTI 类名字符串有 data ref 但**无 code ref**
- RTTI name 布局不符合 MSVC 标准 (strings at +0xC 而非 +0x10)
- **"从字符串反查代码路径不可行"** 是早期尝试的通用失败根因
- **例外**: UI 消息 vtable (`MsgShowSubtitle::vftable`) 是 MSVC 标准, 可见明文 qword ref → v3.6 成功的关键

### 4.2 不可 hook 的函数 (调用者过多)
- `sub_140DAC310` — 49+ callers, 遍布任务/NPC/过场
- `sub_14078A040` — 20+ callers, 所有 Msg* (Show/Remove/...) 共用分发器

### 4.3 x64dbg 反调试
- `0x1420E66DC` 处 `mov [0], 0xDEADCA7` 断言 trap, 条件断点会触发 crash
- NOP 掉 trap + HWBP 路线不稳 (MCP 掉 attach, RVA 漂移)
- v3.6 之后**纯 mod 侧 MinHook 完全绕开调试器**, 不再纠缠

### 4.4 字幕文本不驻留 CPU 内存
- Decima 用 hash-id → 本地化池 → GPU 渲染, 进程内存 grep 字幕原文零匹配
- HWBP-write-on-buffer 路线作废
- **但 hash-id 本身驻留在消息 payload** → v3.6+ 过滤依据

### 4.5 Wwise 静态分析死胡同
- Event ID 运行时 bank 加载, 二进制里 0 常量
- Wwise hash 算法未知, FNV 变体全失配
- `.bnk` / `.pck` 不在磁盘, Decima 自家格式重打包
- **静态分析已探底, 后续只能运行时 probe**

## 5. v3.8 → v3.10: producer 层 identity probe

### 5.1 思路转折
v3.6 ShowSubtitle hook 灭全字幕 = 核弹式屏蔽, 不是精准过滤。真实目标: **只屏蔽 dollman, 保留 Sam/NPC/任务字幕**。策略: 上游 producer (`sub_1403873E0`) 层找 speaker-specific key。

### 5.2 identity 定位
probe 入口 deref `*(*(this+200)+72)` 得到 "pre_source identity" 对象, 读前 8 qword:

```c
words[0]       = vtbl
words[1] >> 32 = speaker/voice-category tag (hi32)   ← 关键字段
words[2..3]    = 跨 session 稳定的 persistent voiceset hash
```

**重要修正 (v3.12 实测推翻早期推断)**: hi32 **不是纯 speaker ID**, 而是 voice category tag。同一 speaker 在不同场景会走不同 tag (例: dollman 在野外休息 vs throw 触发不同 hi32)。

### 5.3 踩过的坑

| 坑 | 表现 | 修复 |
|---|---|---|
| identity cache bug (v3.9) | 32 slot 表满后 `already=FALSE` 但不加入, 同 ptr 反复 log 80+ 次 | 扩到 4096 + 满了强制 stop + 一次性 warn |
| F7 按住式 mark 污染 | 按住 1 秒覆盖 ~10 次 hook 调用, 短语音被错标 | v3.10 latch: 4 键边沿触发 (开始/结束分开) |
| 开始键互斥 | other/dollman 同时 mark, 污染分类 | v3.11 互斥 + F12 清空 |
| 单实例冲突 | 重注入撞 mutex 静默失败 | v3.11 init log 显式报 |

### 5.4 分析脚本 (`tools/`)
- `identity_summary.py` / `identity_dist.py` — 分类/分布矩阵
- `identity_drill.py` / `identity_verdict.py` — 指纹详情 / 跨类冲突判定
- `identity_timeline.py` / `session2_timeline.py` — 时序对照

## 6. 字幕分类全景 (v3.15 实测更新)

### 6.1 两个维度 (必须分开记录)

**按上游通路分 (架构层,v3.15 收敛后)**:

| 通路 | 上游 hook | 特征 | ShowSubtitle caller |
|---|---|---|---|
| ① DSOnTileEntityVariableComponentResource 主链 | A/B/C 三 builder + producer | 可读 hi32 tag,走 EntityMessaging dispatcher,**显式 emit 已收敛为 5 个 selector (`0x11 / 0x12 / 0x17 / 0x14 / 0x05`)**,剩余未知收敛在 case6/7/8 control window | `0x388037` |
| ② DSRadio / dollman random | 0xc7293a (sub_140C724E0) | random chatter, 独立通路 | (同 ①,最终汇合) |
| ③ `DSTalkManager::StartTalk` 剧情路径 | 未 hook | 休息室对话, 无 hi32 | 未知, 待 probe |
| ④ 死亡/任务失败 | 未 hook | 死亡 category payload (q4/q5) | `0x202eefe` |

**按运行时观测分组 (不要和架构根轴混用)**:

| 层 | 当前观测 | 说明 |
|---|---|---|
| 架构根轴 | `selector 0x11 / 0x12 / 0x17 / 0x14 / 0x05` + case6/7/8 | gameplay direct emit 真正固定下来的 sentence-group 选择轴 |
| hi32 摘要 | `0x1f4 / 0x222c / 0x4377 / 0x3745 ...` | producer / SoundHelper 侧摘要,可做 probe / 热键,**不是**最终 family 轴 |
| 热键分组 | J/K, N/M | 手工 mute 组; **J** 已确认覆盖 throw/recall/换帽/换眼镜/野外休息, **N** 至少覆盖拿出狙击枪 |
| 非目标 / 未覆盖 | 休息室对话 / 任务失败 | 分别在通路 ③ / ④,不应混入 gameplay direct 统计 |

### 6.2 当前 gameplay direct 观测值

| 维度 | 当前值 | 结论 |
|---|---|---|
| explicit selector | `0x11 / 0x12 / 0x17 / 0x14 / 0x05` | 已静态坐实; 下一步是把具体动作域逐个映射到这 5 个 selector，并补 case6/7/8 的 pending 命中面 |
| dollman gameplay hi32 | `0x1f4 / 0x222c / 0x4377 / 0x3745` | 都已在 gameplay direct 样本里出现; `0x3745` 是新增样本,具体行为标签待补 |
| other hi32 | `0x7993 / 0x766b / 0x766f` | 目前对应其他人物 / 主角样本,用于排除误伤 |

### 6.3 过时表述 (v3.12 前)

以下说法现在都应视为**过时**:

- 把 `0x1f4` 直接等同于 "dollman"
- 把 `0x222c / 0x4377` 直接等同于某个固定行为域
- 把 J/N 热键分组直接当成 gameplay 架构 family
- 认为 throw/recall 走 builder B (`MsgStartTalk`)

v3.15 实测进一步证实:hi32 不是 speaker ID,而是 **SoundHelper 缓存摘要**(由 0x3541c0 从上游灌入,同一 speaker 不同场景得到不同 tag)。同时新增观测到 gameplay hi32 `0x3745`,进一步说明旧的 `0x1f4 / 0x222c / 0x4377` 二分表已经不够。

另: **throw/recall 实测走 builder C (`MsgDSStartTalk`),不是之前推测的 B (`MsgStartTalk`)**。static 画像 "C = 带 sequence/lipsync/facial 资源" 与之吻合。

### 6.4 待 probe 解决
- `selector 0x05 / 0x11 / 0x12 / 0x14 / 0x17` 各自到底对应哪些 gameplay 域?
- throw / recall / 野外休息 / 换帽 / 换眼镜 / 狙击枪切换 分别落在哪个 slot?
- case6/7/8 里哪些 pending 会真的命中 `+0x660 / +0x6B0 / +0x700` 的有效 cell?
- 新增 hi32 `0x3745` 属于哪个 slot?对应哪个具体动作域?
- `sub_140F04620` 里的 event hash → pending → slot 映射,还需要把 likely 项逐个钉成实锤
- `sub_140350460` 前半段的 `source / wrapper / v13` 选择到底对应哪类 gameplay 语义?
- `sub_140C730B0 / sub_140C741E0 / sub_140C724E0` 的上游 caller 链里,谁第一次决定 `speaker / entity / player / dollman`?
- `sub_140B0DB60 / sub_141EBEA60 / sub_141EBBFC0 / sub_140FF6B60` 的真实运行期使用面还有哪些尚未归类?
- msg_vtbl_rva 能否稳定区分 MsgExpressSignal/StartTalk/DSStartTalk?(v3.15 dump 已就绪)
- 休息室对话(通路 ③)上游 hook 点

## 7. 架构演进: v3.13 → v3.15 热换 + 文件触发

### 7.1 动机
v3.12 之前改任何一行要关游戏 → 重编 → 重启, 单次迭代 1~2 分钟, 打断实机调试节奏。v3.13 拆成两层, F10 可热换 core 而不重启游戏。

### 7.2 架构

```
DollmanMute.asi  (proxy, 常驻)
  ├─ src/proxy_main.c          不链 MinHook
  ├─ DllMain → bootstrap 线程 → LoadLibrary(core) → F10 监听
  ├─ 持有 mutex Local\DollmanMuteProxyInstance
  └─ 日志: DollmanMute.proxy.log (load/unload/reload 事件)

DollmanMuteCore.dll  (core, 可热换)
  ├─ src/core_main.c           所有 hook / hotkey / probe 逻辑
  ├─ 导出 core_init(const ProxyContext*) / core_shutdown(void)
  └─ 日志: DollmanMute.log

src/core_api.h                 ProxyContext + 导出原型
```

### 7.3 关键设计决策

| 决策 | 理由 |
|---|---|
| mutex 归 proxy | core reload 若在 core 里 acquire 会 `ERROR_ALREADY_EXISTS` 自杀 → 必须进程级持有 |
| MinHook 只在 core | MinHook 用全局 static 状态, 两 DLL 各链一份 = 双状态冲突 |
| proxy DllMain 不 LoadLibrary | loader lock 下不宜做重事 → 启动 bootstrap 线程, 在新线程里 Load + init |
| 栅栏无需 Sleep | `MH_DisableHook` 内部 `Freeze()` 已 SuspendThread 所有其他线程 + 检查 PC 不在 trampoline |
| TlsFree 必须配对 | TlsAlloc slot 是进程级资源, FreeLibrary 不回收 → 每次 reload 泄 1 slot |
| CriticalSection 移入 init/shutdown | reload 不走 DllMain, 必须配对 init/delete |

### 7.4 core_shutdown 严格顺序

```c
1. InterlockedExchange(&g_core_shutting_down, 1)  // 信号
2. WaitForSingleObject(hotkey_thread, 500)        // 停线程 (超时 TerminateThread 兜底)
3. MH_DisableHook(MH_ALL_HOOKS)                    // 卸 hook (MinHook 内置 Freeze 栅栏)
4. MH_Uninitialize()                               // 释放 trampoline
5. 清 g_real_* / identity cache / hotkey prev flags
6. TlsFree(g_tls_muted_subtitle_family)
7. DeleteCriticalSection x3                        // 最后删
```

顺序不可换: 颠倒会踩已释放内存 / 锁残留 / 无效 TLS slot。

### 7.5 build.ps1 用法 (v3.15 增强)

```powershell
./build.ps1              # 全量: proxy + core (关游戏时用, 或首装)
./build.ps1 -CoreOnly    # 只编 core (游戏运行时用,自动处理 unload/copy/load)
```

**v3.15 起 `-CoreOnly` 无需手动干预**:若磁盘 core 被锁,脚本自动 touch `DollmanMute.unload` → 轮询最多 6 秒等解锁 → copy → touch `DollmanMute.load`。全程不用 alt-tab。

### 7.6 proxy 文件触发 (v3.15 新增)

proxy 的 reload 线程每 ~250ms 轮询 3 个 flag 文件,发现后 consume + 执行:

| 文件 (game root) | 行为 |
|---|---|
| `DollmanMute.reload` | 原子 unload + load (= 一次 F10) |
| `DollmanMute.unload` | 仅 unload (磁盘锁释放) |
| `DollmanMute.load` | 仅 load (需要在 unload 状态) |

F10 按键仍然保留作为应急(等价 `.reload`)。

### 7.7 热换工作流

**改 core 代码** (v3.15 简化):
1. `./build.ps1 -CoreOnly` — 一键搞定 (脚本自动触发 unload/copy/load)
2. 验证 `DollmanMute.log` 出现新 `k_build_tag`

**改 proxy 代码**: 仍必须关游戏 → `./build.ps1` → 重启

**日志诊断**:
- `DollmanMute.proxy.log`: `core_init returned 0` / `core unloaded cleanly` / `File trigger: *.unload/load/reload consumed`
- `DollmanMute.log`: `DollmanMute build: ...` / `core_shutdown begin/complete` / `=== session boundary F8 count=N ===` / `=== log cleared by F9 ===`

### 7.8 坑位提醒

1. 改 hotkey / hook / probe = core, 可热换
2. 改 F10 / proxy mutex / 文件触发 = proxy, 必须关游戏重启
3. 别在 ShowSubtitle handler 里加 Sleep (MinHook 内置 Freeze 已够)
4. 不要在 DllMain 里 LoadLibrary (loader lock)
5. 两个 mutex 名不要混: 旧 `DollmanMuteProbeSingleton` (已删) vs 新 `DollmanMuteProxyInstance`
6. `ProxyContext.proxy_version` 破坏性变更需 bump, core 据此拒绝不兼容 proxy
7. flag 文件命名不能冲突: `DollmanMute.unload/load/reload` 是 proxy 保留名

### 7.9 v3.14 builder probe + v3.15 msg payload dump

**目的**: 定位 dispatcher + 把 dollman 动作映射到 builder 路径。

**新增 hook (log-only, ini `EnableBuilderProbe=1`, 默认 TRUE)**:
- A/B/C/U1/U2 = sub_140350050 / 460 / 960 / b5bc70 / b5cc30
- 每次命中 log: `[builder=X] this=... arg2=... caller_rva=... msg_vtbl_rva=... msg=[w0..w7]`
- caller_rva 用 `__builtin_return_address(0)` 抓 dispatcher 的 call-site;因为 thunk 只是 jmp 不压栈,RA 直通 dispatcher
- msg payload dump 读 rdx 指向的前 0x40 字节 (栈上 msg obj),`IsBadReadPtr` 保护

**v3.15 新增 hotkey**:
- **F8** — 写一行 `=== session boundary F8 count=N ===` 作 log 分隔符
- **F9** — `CreateFileA(CREATE_ALWAYS)` 截断 `DollmanMute.log` 到 0 字节

**TLS 关联已知缺陷**: core 用 TLS slot `g_tls_last_builder` 记最后进入的 builder id,`hook_show_subtitle` 读它写进 log。实测 builder 和 ShowSubtitle 在不同线程 (tid mismatch),TLS 失效,`builder=none` 常见。事后靠时间戳 + session 切片聚合,比线程内 TLS 关联更可靠。

## 8. 关键文件 / 速查

### 8.1 源码
- `mods/DollmanMute/src/proxy_main.c` — ASI 入口, F10 + flag 文件监听, LoadLibrary
- `mods/DollmanMute/src/core_main.c` — 所有 hook 逻辑 (原 dllmain.c 改造)
- `mods/DollmanMute/src/core_api.h` — proxy/core 共享接口
- `mods/DollmanMute/build.ps1` — 双 target 构建, `-CoreOnly` 自动 unload/copy/load

### 8.2 运行时
- Game root: `C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH`
- Log: `DollmanMute.log` (core) + `DollmanMute.proxy.log` (proxy)
- IDB: `DS2.exe.i64` (ImageBase `0x140000000`)
- Flag 文件: `DollmanMute.{reload,unload,load}` (proxy trigger)

### 8.3 代码 / 运行时关键常量

> 下表仍保留源码里的历史 `k_rva_*` 名称，方便对照当前代码；它们不是“已经由新 IDA 证明的真实函数起点”。

| 常量 | 值 |
|---|---|
| `k_rva_dollman_radio_play_voice_by_controller_delay` | `0x00C73DF0` |
| `k_rva_dollman_voice_dispatcher` | `0x00DAA410` |
| `k_rva_subtitle_runtime_wrapper` | `0x00780690` |
| `k_rva_show_subtitle` | `0x00780740` |
| `k_rva_remove_subtitle` | `0x00780840` |
| `k_rva_subtitle_producer` | `0x003872C0` |
| `k_rva_builder_a` (MsgExpressSignal) | `0x00350050` |
| `k_rva_builder_b` (MsgStartTalk) | `0x00350460` |
| `k_rva_builder_c` (MsgDSStartTalk) | `0x00350960` |
| `k_rva_selector_dispatch` | `0x00DAF8A0` |
| `k_rva_talk_dispatcher` | `0x00385720` |
| `k_rva_builder_u1` | `0x00B5BC70` |
| `k_rva_builder_u2` | `0x00B5CC30` |
| gameplay direct explicit selector | `0x11 / 0x12 / 0x17 / 0x14 / 0x05` |
| gameplay direct 已观测 hi32 | `0x01F4 / 0x222C / 0x4377 / 0x3745` |
| other 已观测 hi32 | `0x7993 / 0x766B / 0x766F` |

### 8.4 vtable / 静态地址参考
- 旧 `0x143188E48 / 0x143188E88 / 0x143188E90` 这一组已经不再能当当前 build 的字幕入口真锚点；`0x143188E88` 在 live 对应的 raw 值不是当前字幕入口。
- 当前更可信的 `GameViewGame` RTTI table group 落在 `0x143188DA0` 附近；相邻槽位 `0x143188DE0 / 0x143188DE8` 对应当前 subtitle show/remove 簇。
- 当前更可信的 show sender 是 `sub_140780740`；配对 remove sender 是 `sub_140780840`。
- UI queue 真函数当前更适合记成 `sub_140789C80`；`0x14078A040 / 0x14078A070` 都按它的内部 callsite 理解。
- `MsgShowSubtitle::vftable` 明文 qword ref 仍可作为 UI 字幕消息构造证据使用。
- `0x140160ED0` — **EntityMessaging dispatcher** (dispatch 指令 @ `0x14016107f`, caller_rva 落点 `0x140161082`)
- `0x1441fa7c0` — DSOnTileEntityVariableComponentResource handler table 起点 (10 条 slot × 16B)
- `0x143135930` — `DSTalkInternal::StartTalkFunction` vtable
- `0x143132678` / `0x143132688` — `DSTalkComponentRep::SoundHelper` primary / secondary vtable
- `0x143135548` / `0x1431354B8` — `Simple/DummySoundInstanceWrapper` vtable
- `0x1431358E0` / `0x143135368` — `NotificationQueueImpl` primary / secondary vtable

### 8.5 当前 build 突破点
- 新 build 下更可信的字幕 UI 三件套已经收敛成: `sub_140780740` = show sender, `sub_140780840` = paired remove sender, `sub_140789C80` = manager / queue hub；旧 `sub_140780710` 不再当真入口。
- `0x00DAFAD0` / `0x003857E0` 这两个旧 probe 偏移都已经失效；它们只是父函数内部中段。当前更可信的替代入口分别是 `sub_140DAA410 -> sub_140DAF8A0` 和 `sub_1403881B0 -> sub_140385720`。
- payload 当前工作结论按结构视角统一记成: `+0x00 = body`, `+0x08 = speaker`；旧文里 `a1[1] = speaker`, `a1[2] = text/body` 的写法按“上层 holder 里的等价观测”理解即可，不再把 `0x1403857E0` 当独立入口。
- `manager + 0x8F0` 的三路 subscription table 是当前更稳的字幕 UI 汇合层；`0x14078A070` 应继续按 `sub_140789C80` 的内部 callsite 理解，不再单独拔高成入口。
- gameplay 侧更稳的统一层仍是 `EntityMessaging` 分发簇；除 `0x140160ED0` 外，`0x140161430` 也应按当前 build 的稳定统一层 / bridge 理解，而不是继续执着旧 `SelectorDispatch` / `TalkDispatcher` 偏移。
- 当前更稳的 identity / family surface 已经不是旧 dispatcher 偏移，而是 RTTI 干净的 `DSRadioSentenceGroupThrough*Instance` 三兄弟：
  - `sub_140C72170` -> `ThroughSpeakerInstance` 写 family `2`
  - `sub_140C72140` -> `ThroughPlayerInstance` 写 family `3`
  - `sub_140C72150` -> `ThroughDollmanInstance` 写 family `4`
  这组值比旧 live offset 更适合当“Dollman / Player / Speaker 分流”的长期稳定锚点。
- `family=2/3/4` 的 numeric 落点当前也已经出现，不必再假设它藏在 `sub_140C730F0 / sub_140C74220` 内部：
  - `Speaker vs ThroughEntity` 主要靠 vtable / 专用方法分流：`sub_140C74220` 是 speaker 专用，`sub_140C730F0` 是 player/dollman 共享的 `ThroughEntity`
  - `Player vs Dollman` 再往下主要靠 `sub_140C739B0` vs `sub_140C73E30` 这对 delay wrapper 分开
  - 真正把类型值写成 `2/3/4` 的显式 switch 在 `sub_140C72520 @ 0x140C72944`，分支源是 `*(_BYTE *)(*(_QWORD *)(_RBX + 40) + 88)`，映射目前看到是 `0 -> 1`, `1 -> 3`, `2 -> 4`, `3 -> 2`, `4 -> 5`
  - 这个值随后会在 `sub_140388BE0 @ 0x140388C60` 被写入 `StartTalkFunction + 0x58` 这一类 talk 启动状态字段
- `ThroughEntity` 和最终字幕 sender 之间现在已经有实锤 join 链，而不只是“像是同一家族”：
  - `sub_140C72520` (entity 侧) 会直接调用 `sub_140388BE0` (`DSTalkInternal::StartTalkFunction` ctor)
  - `sub_140388BE0` 会把输入结构 `+0x10..+0x1F` 的 16 字节 pair 原样拷到 `this+0x48`
  - `sub_140387670` 再组一份 4 槽块到 `this+0xE8`
  - `sub_1403881B0 -> sub_140385720 -> sub_140385A70` 把这份 payload 推到 observer / subtitle 面
  - `sub_140780740` 只负责把 incoming pair 原样写进 `MsgShowSubtitle`
- `sub_140385A70` 当前应视为真正的 reorder seam：它在下游回调前明确把上层 4 槽块里的 `slot2 / slot1` 重排成最终 outbound 顺序；因此 `ShowSubtitle` 看到的 `+0x00/+0x08` 不是在 `GameViewGame` 侧决定的，而是在 talk/listener 这一层决定的。
- `Speaker` 线和 `ThroughEntity` 线已经能静态区分成两套 producer 面：
  - `Player` / `Dollman` 共享 `sub_140C730F0` + `sub_140C733E0`，会懒创建 `DSTalkInternal::NotificationQueueImpl`，并走 `sub_140C72240` gating
  - `Speaker` 走 `sub_140C74220` + `sub_140C74560`，也是 queue 驱动，但没有看到它复用 `sub_140388BE0`
  结论上应把 `ThroughEntity` 当统一 producer seam，把 `Speaker` 当独立旁路，而不是再把三者混成同一条老 dispatcher 路线。
- `sub_140789570 / sub_140789780 -> sub_140161AD0 -> sub_140161720` 这条链已经证明 `manager + 0x8F0` 不只是“像注册表”，而是真正在按 message-type 建 handler：
  - 目标表起点就是 `+931408`
  - paired lock 起点对应 `+931528 / +931568 / +931608`
  - type index 来自 message 对象的虚函数 `[*+24]`
  - `sub_140161720` 按 message type key 做二分查找 / 插入，必要时扩 handler 槽位
  - metadata record 当前可按 `24` 字节结构理解：`{ msg_desc, handler_thunk, sink_offset, unk_14 }`
  - message type key 的真实来源不是 record 自己，而是 `*(u16 *)msg_desc`
  - 实际写进 handler slot 的内容更像 `{ (sink_base + sink_offset) | 1, handler_thunk }`
  - 当前工作位域解释是：entry metadata 的低 14 bit 更像 `first handler index`，`>> 14 & 0x3FF` 更像 `num handlers`；这和 `MAX_FIRST_INDEX = 0x4000` / `MAX_NUM_HANDLERS = 0x400` 两条 overflow 字符串是对上的
  - `send_count` 也基本能按 `(packed >> 24) & 0x7F` 理解；`bit31` 更像 dirty / in-send / 待压缩标记
- `0x14078A070` 这族 HUD / subtitle message 的工程上可用层级，当前更适合记成：
  - 顶层共祖：`MsgBase`
  - 操作性中间层：`MsgHUDUpdate` (`0x14427C860`)
  - 下层 concrete：`MsgShowSubtitle` / remove / popup / combat reward 等
  其中 `descriptor +0x58` 并不能稳定给出一棵干净继承树；目前唯一明显露出 named intermediate 的，是 `MsgHUDShowPopup -> MsgHUDShowNotification`。
- `MsgShowSubtitle` 的 descriptor lineage 当前可以稳定记成：`sub_140780740 -> MsgShowSubtitle::vftable (0x143188EF0) -> getter sub_14077EF80 -> descriptor 0x144279670`。这条链比继续追旧 UI sender 偏移更可靠。
- `LocalizedTextResource` 这条线已经出现两个业务层入口，不再只是 RTTI / 注册代码：
  - `DSUINodeGraphBindingsNS::GetStringFromLocalizedText`: `sub_14145FA30 -> sub_14146B640 -> sub_14145F540 -> sub_1426F6A70`
  - `HUDText::SetLocalizedTextResource`: `sub_140786290 -> sub_14078FF00 -> sub_140791F60 -> sub_1426F6A70 -> sub_1407872A0`
  这说明当前 build 里 `sub_1426F6A70` 已经是很强的 `LocalizedTextResource -> runtime string` extractor 候选，不必再只盯旧 live 偏移。
- `sub_1426F6A70` 现在已经能当“当前 build 的直接字段 reader”记下来，而不只是候选：
  - `0x1426F6A99  mov rdx, [rdi+20h]`
  - `0x1426F6AB1  movzx r8d, word ptr [rdi+28h]`
  - `0x1426F6ABB  call sub_1400A3FD0`
  这和旧 live 结论 `+0x20 = text ptr`, `+0x28 = length` 是对得上的。
- `sub_1426F6B20` / `sub_1426F6C90` 是同一 extractor 旁边的格式化 helper：它们先走 `sub_1426F6A70` 取字符串，再拼 `[...]` 或键值样式输出，更像 debug / UI formatting 面，不像资源本体。
- 因此 `LocalizedTextResource` 这条线当前可以分两层记：
  - 业务层 choke point：`sub_14145F540` / `sub_140791F60`
  - 字段级 direct reader：`sub_1426F6A70`

## 附录 A: Signature 漂移应急 (游戏更新后)

若 J/N 热键全失效 (hi32/tag 值被游戏改动):

1. ini 打开 `EnableSubtitleProducerProbe=1` 和 `EnableBuilderProbe=1`
2. core 记录 producer `words[0..7]` + builder `msg_vtbl_rva + msg[0..7]`
3. 实机走 J 组行为 (throw/recall/换帽子/换眼镜/野外休息) + N 组行为 (狙击枪切换) 各 3 次, 用 F8 切 session
4. 先确认 gameplay direct 样本是否仍收敛在显式 selector `0x11 / 0x12 / 0x17 / 0x14 / 0x05`
5. 再从 log 读新 hi32 值 (`words[1] >> 32`) 和 builder 分布
6. 若只是 hi32 漂移,更新 core_main.c 里的 family/tag 判定; 若 slot 集合也变了,回到 `sub_140F000B0 / sub_140DAFAD0`
7. `./build.ps1 -CoreOnly` 热换验证

若 dispatcher/handler table 地址漂移:

1. 抓任意 builder 命中,读 log 里 `caller_rva` → 找新 dispatcher
2. 从 dispatcher 内部 `call qword ptr [rax+8]` 之前找 rax 构造 → 定位新 handler table
3. 更新 `k_rva_builder_*` 常量 (可通过 IDA xref thunk → handler 间接得到)

## 附录 B: 未测试但有线索的候选路径

### B.1 Speaker PVCD (`sub_140C741E0`)
4 个 Instance 变体里，`Speaker` 线现在已经静态扩图到 `0x140C790A0 -> 0x140C741E0 -> 0x140C73FF0`，但仍缺 runtime probe。828 字节 (Dollman PVCD 的 5 倍),函数体内直接构造 `DSTalkInternal::NotificationQueueImpl` (RESEARCH 早期标注的字幕可疑类)。

双分支:
- `a3 == 0`: closure 入队 `this + 8168` (和 Dollman PVCD 对称)
- `a3 != 0`: 遍历 `*(int*)(this+48)` 批量 dispatch 经 `sub_140388E60`

**若验证**: 先 log-only 3 次 throw/recall 看命中 + a3 分布, 再决定拦截策略。**不要一上来 return 0** (避免再次 "0 hits" 翻车)。

### B.2 DSTalkManager 上游 hook (通路 ③)
RTTI `StartTalk@DSTalkManager@@` @ `0x1460BE4DC`。Decima hash RTTI 让直接 xref 失败, 需走 COL 反推 (参考 Speaker vtable 的反推方法)。若 hook 上, 应同时 kill 休息室对话字幕 + 音频。

### B.3 其余 7 条 handler table slot
table @ `0x1441fa7c0` 里除了 A/B/C (slot 5/6/7) 和前 5 条的 `sub_140354CC0/C60/BD0/B20/AE0` + 尾 2 条 `sub_1403549C0/960`。这些 thunk 都是 2-arg __fastcall,装 log-only hook 的成本同 A/B/C,若 J/N probe 有漏网行为可一次补齐 5 条。

### B.4 `0x140C79A90` / `sub_140C79480` 新 caller 簇
`sub_1403896f0` 相关的新 caller 不再只看孤立的 `sub_140C79480`；当前更值得盯的是它的 cluster head `0x140C79A90`,下面挂着 `0x140C79480 / 0x140C79570 / 0x140C79640 / 0x140C799B0` 这一簇。它们现在应当视作 `DSRadioSentenceGroups` 旁路线的第二批探针,优先级已高于旧的 “U1/U2 未定类” 假设。

### B.5 2026-04-26 语音路径纠偏
- 旧源码常量 `k_rva_dollman_radio_play_voice_by_controller_delay = 0x00C73BF0` 在当前 build 上**不是** live 的 delay wrapper；它实际落在 `sub_140C73BF0`，更像 `DSRadioSentenceGroupThroughDollmanInstance` 的 teardown / cleanup 面。
- 当前更可信的 Dollman 专属语音延迟链应记成：
  - `sub_140C73E30` — `PlayVoiceByControllerDelay_DSRadioSentenceGroupThroughDollmanInstance` delay wrapper
  - `sub_140C7E780` — 纯 thunk
  - `sub_140C73EE0` — Dollman delay closure 真执行体
  - `sub_140DAC7B0` — Player / Dollman 共享的 voice-event 提交 helper
- Player 对照链是：
  - `sub_140C739B0 -> sub_140C7E760 -> sub_140C73A60 -> sub_140DAC7B0`
- `sub_140DAC7B0` 当前两个已确认代码调用点：
  - Player: `0x140C73AE9`
  - Dollman: `0x140C73F68`
  这说明它是共享 helper，不适合无条件拦。
- 当前最窄的 Dollman-only 语音切点优先级：
  1. `sub_140C73EE0` 早退 / no-op
  2. `sub_140C73E30` 早退
  3. `sub_140DAC7B0` + 仅按 Dollman caller 判别
- 需要保留的风险面：`ThroughEntity` 已经能解释当前成功的 sender-only 字幕静音，但残余 Dollman 语音**不一定**还走同一路。
  当前更像独立旁路的 Speaker 线是：
  - `sub_140C74220` — `PlayVoiceByControllerDelay_DSRadioSentenceGroupThroughSpeakerInstance`
  - `sub_140C7E7A0` — delayed closure fan-out 到 `qword_146230F40 + 128` listener 列表
  这条线静态上足以解释“字幕没了但语音还在”，若 Dollman 专属 closure mute 无效，应优先回到这条 Speaker 旁路做窄 probe。

# DollmanMute 研究笔记

> 最后更新: 2026-05-13
> 当前源码 build tag: `v3.0-dynamic-resolution-dev`
> 当前目标版本: DS2 v1.7.76
> 详细静态总图见:
> `C:\Users\Administrator\Downloads\dbg\ds2ida\DS2V1.7\DS2_V1.7_AUDIO_SUBTITLE_MAP.md`

这份文件只保留当前仍能指导代码和实验的事实。旧偏移、旧 probe 史、v1.6/v2.1.17 之前的叙事只作为 git 历史，不再混入当前判断。

## 0. 目标

唯一目标没变:

- **只屏蔽 Dollman 的 gameplay 语音和字幕**
- **尽量不误伤 Sam / NPC / cutscene / private-room / 其他系统**
- **不要把“能启动”或“某一次没听到声音”误当成完成**

当前新增研究目标:

- 不只知道“消费了一条 Dollman voice queue”
- 还要尽量知道具体消费了什么内容: 资源指针、句子索引、文本 tag、sound resource id 或其他稳定身份

## 1. 当前运行时形态

当前 game-root `DollmanMute.ini` 关键项:

```ini
[General]
Enabled=1
VerboseLog=0
EnableVoiceMute=1
EnableSubtitleMute=1
ScannerMode=0
HookRadioDispatcher=1
HookEchoback=1
HookQueueStartTalk=1
HookStartTalkUpdate=1
HookTalkSoundWrapper=1
HookDollmanVoiceSchedule=1
HookDollmanVoiceClosure=1
HookVoiceSharedHelper=1
HookVoiceQueueSubmit=0
EnableVoiceQueueIdentityProbe=0
ToggleHotkeyVK=119
```

当前常驻 hook 面:

| 面 | v1.7 RVA / 来源 | 当前角色 |
|---|---:|---|
| RadioVoiceDispatcher | `0x140C735C0` / Dollman vtable[2] | 旧 talk/starttalk 链路的 Dollman 标记面，仍保留观察与兼容价值。 |
| Echoback enqueue/execute | `0x140388200` / `0x140387490` | 旧 deferred talk 链路。当前 random Dollman 不主要靠这里闭合。 |
| QueueStartTalkFunction | `0x140388C20` | 旧 StartTalk 字幕/语音链路入口。 |
| StartTalk UpdateAdvance | `0x1403876B0` | 旧 StartTalk 生命周期门。 |
| TalkSound wrapper | `0x1403899C0` | 旧 StartTalk 声音实例替换点。 |
| Dollman delay schedule | `0x140C74300` | 当前 random Dollman gameplay voice 的关键上游入口。 |
| Dollman delay closure | `0x140C743B0` | 当前最硬的 random Dollman-only voice TLS 标记点。 |
| VoiceSharedHelper | `0x140DACCD0` | 当前早期消费 random Dollman voice queue 的点。必须由 Dollman closure TLS 约束。 |
| VoiceQueueSubmit | `0x140DACE30` | 默认不安装的 identity probe；只在 Dollman TLS 诊断模式下记录 request，不是 release 默认 mute 点。 |

当前热键:

- `F8` / VK `0x77` = 运行时启用/禁用整个 mod
- 暂时不要 MessageBox，避免影响游玩和测试节奏

## 2. 当前已确认事实

### 2.1 random Dollman voice 主链

当前 gameplay random Dollman 语音已经通过 live log 和 IDA 对齐到这条链:

```text
DSRadioSentenceGroupThroughDollmanInstance.vtable[8] 0x140C74300
  -> delay invoke wrapper 0x140C7EC50
  -> DSRadioVoiceDelayClosureSibling_Candidate 0x140C743B0
  -> VoiceSharedHelper_Candidate 0x140DACCD0
```

运行证据形态:

```text
[dollman-voice-schedule] ...
[dollman-voice-closure] ...
[voice-helper] consumed=... dollman ... event=0
```

解释:

- `0x140C74300` 分配 0x20 的 closure parameter set。
- 传给 `0x140C743B0` 的有效 payload 是 closure 对象的 `+0x10`。
- payload `+0x00` 是 `DSRadioSentenceGroupThroughDollmanInstance*`。
- payload `+0x08` 是 controller index。
- `0x140C743B0` 从 group instance 读取:
  - `self+0x10` -> `DSRadioSentenceGroupThroughDollmanResource`，作为 `VoiceSharedHelper` 的 voice source 参数传入
  - `self+0x20` -> notification queue
  - 然后调用 `VoiceSharedHelper`。

### 2.2 旧 voice 链路的降级

旧文档和旧代码里常见这条链:

```text
0x140C73E80 / 0x140C73F30
  -> 0x140DACCD0
```

当前结论:

- 这条链仍然是 sibling/player 对照路径，有结构参考价值。
- 但它不是当前已证明的 random Dollman gameplay mute 主路径。
- 不能再把 `0x140C73F30` 写成当前 random Dollman 的权威 closure。

### 2.3 VoiceSharedHelper 的边界

`0x140DACCD0 VoiceSharedHelper_Candidate` 是共享 helper，不能 blanket mute。

当前安全用法:

- 只在 `0x140C743B0` closure 设置的 Dollman TLS 内消费。
- early return `1`，不调用原 helper。
- 这会在 `VoiceQueueSubmit` 之前消费队列，所以 hook 点看到的 `event_id` 通常还是 `0`。

因此:

- `event=0` 不是 hook 失败。
- 它说明我们拦得早，具体 Wwise event/request 还没生成。
- 要知道“屏蔽了什么内容”，必须继续往上游 group instance / source resource / resource refs 摸，而不是指望这个 hook 点直接给最终 eventId。

当前静态边界:

- `VoiceSharedHelper` 里的 request descriptor 是现场合成的，不是一个已经完整存在的语义对象直传下来。
- request `+0x00` 来自传入的 `sentence_key`；为 0 时才按 output type 填 fallback id。
- request `+0x10 = -1`、`+0x24 = -1` 是默认合成状态，不能直接当内容身份。
- 当前能稳定证明的是 caller/closure 路径身份，不是最终内容 id。

`0x140C743B0` 到 helper 的具体传参:

- closure payload `+0x08` 先写入局部 `v17`。
- `&v17` 作为 `VoiceSharedHelper` 第 5 参数传入。
- helper 读取 `*a5`，写成 request descriptor `+0x00`。
- 因此 `voice-queue-identity id=` 能验证 `controller_index` 是否就是具体 request id；如果 `controller_index=0`，helper 会按 output type 写 fallback id。

### 2.4 VoiceQueueSubmit 诊断边界

`0x140DACE30 VoiceQueueSubmit_Candidate` 已接成默认关闭的 identity probe:

- `EnableVoiceQueueIdentityProbe=0` 时，行为不变：Dollman TLS 内 `VoiceSharedHelper` 直接早退 `return 1`。
- `EnableVoiceQueueIdentityProbe=1` 时，Dollman TLS 内允许 `VoiceSharedHelper` 继续构造 request descriptor。
- 随后 `VoiceQueueSubmit` hook 只在同一个 Dollman TLS 内记录 request / owner 字段，并写 `out_status=0`、`return 1`，阻止真正入队。
- 这个模式的目标是拿内容身份，不是 release 默认路径。

Kepler 静态核对结论:

- `VoiceQueueSubmit` 返回 `1` 表示上层认为提交成功/已处理。
- 返回 `0` 才触发 `VoiceSharedHelper` 和 closure 的失败 fallback。
- 第 6 参数 out status 成功语义是 `0`；原函数只在特定失败/冲突分支写 `0x3A`。

开关工具:

```powershell
powershell .\tools\voice_identity_probe.ps1 -Status
powershell .\tools\voice_identity_probe.ps1 -Enable
powershell .\tools\voice_identity_probe.ps1 -Disable
python .\tools\voice_identity_report.py --tail 400
powershell .\tools\watch_voice_identity.ps1 -Tail 800 -EnableProbe -DisableProbeOnExit
```

`-Enable` 会同时写 `EnableVoiceQueueIdentityProbe=1` 和 `HookVoiceQueueSubmit=1`；`-Disable` 会恢复两个值为 `0`。
`voice_identity_report.py` 用来汇总 `[dollman-voice-sentence-group]` / `[voice-queue-identity]`，并提示是否还缺 schedule、sentence group 或 queue identity 样本。
`watch_voice_identity.ps1` 不弹 MessageBox，只轮询 `DollmanMute.log` 并在日志变化时刷新 report；可选 `-EnableProbe` / `-DisableProbeOnExit` 用于一次 runtime 采样会话。

### 2.5 当前 runtime probe 方向

当前只读 probe 已加入/待游戏加载:

- `dollman-voice-schedule`
  - 记录 `self`
  - `self+0x10/+0x18/+0x20`
  - `self+0x40/+0x44`
  - source resource `+0x60/+0x64/+0x68/+0x70/+0x78/+0x80/+0x88/+0x90/+0x91/+0xA0`
  - `source+0x88` 指向的 `SentenceGroupResource+0x20/+0x28/+0x30/+0x38/+0x40`
  - source/playback/queue vtable
- `dollman-voice-closure`
  - 记录 payload、self、controller
- `voice-helper-probe/state`
  - 记录 source resource 的一组低风险字段
  - 记录 helper queue 相关状态
- `voice-queue-identity`
  - 仅 `EnableVoiceQueueIdentityProbe=1` 时出现
  - 记录 request `+0x00/+0x08/+0x10/+0x14/+0x18/+0x1C/+0x20/+0x24`
  - 只读复刻 `sub_140DA8720(owner+0x38, request_id, -1)` 的哈希查找，记录 `catalog_slot/index/entry`
  - 如果解析到 catalog entry，记录 entry vtable/type 和 `+0x08..+0x38` 的 qword 快照
  - 记录 owner `+0x38/+0x40/+0x44/+0x48/+0x60/+0x64/+0x1EF`
- `dollman-voice-sentence-index`
  - 用 `controller_index` 只读尝试索引 `source+0x88 -> SentenceGroupResource+0x30`
  - 如果 in-bounds，记录候选 `SentenceResource+0x30/+0x38/+0x40/+0x48/+0x50/+0x58`
  - 这只是验证 controller 是否能作为句子数组下标，不能预设它一定成立
- `dollman-voice-sentence-scan`
  - 仅 `EnableVoiceQueueIdentityProbe=1` 时额外记录。
  - 扫描 `SentenceGroupResource+0x30` 前 16 个 `SentenceResource`，记录每项 `sound/text/voice_fallback/voice`。
  - 目标是判断 request/controller/catalog key 是否能和句组中的某一条内容身份稳定对齐。

第一条已有 probe 结果:

```text
self+0x10 == VoiceSharedHelper source 参数
self+0x20 == VoiceSharedHelper queue 参数
self+0x18 == 0
event == 0
```

这说明第一次猜测的 `self+0x18` / `source+0x70` 还不是内容身份来源，后续要继续看 resource 字段或更晚的 guarded `VoiceQueueSubmit` request。

### 2.6 DSRadioSentenceGroupThroughDollmanInstance 字段边界

IDA 目前把 instance 的构造点收敛到 `0x140C79430`:

- 分配 `0x48` 字节 instance。
- `+0x10 = DSRadioSentenceGroupThroughDollmanResource*`，并 AddRef。
- 如果 `resource+0x80` 存在，调用其 vtable `+0x20`，结果写到 `+0x18`。
- `+0x20 = 0`，后续由 `0x140C735C0` lazy allocate notification queue。
- `+0x40/+0x44` 是 runtime counter/state flag。

字段判断:

| 字段 | 当前判断 | 内容身份价值 |
|---|---|---|
| `self+0x10` | Dollman sentence group resource，传给 `VoiceSharedHelper` arg2 | 最高；稳定来源，但不是单句 |
| `self+0x18` | 来自 `resource+0x80` vcall，和 `PlaybackEvent` 查询有关 | 中等；更像 playback graph / playinfo metadata |
| `self+0x20` | notification queue | 低；队列/状态，不是内容身份 |
| `self+0x40/+0x44` | active counter / state flag | 低；runtime 状态 |

source resource 字段来自 type descriptor:

| 字段 | 当前判断 |
|---|---|
| `source+0x60/+0x64` | `DSRadioBaseResource` 的基础调度/禁用类字段 |
| `source+0x68/+0x70` | `Array_Ref_BooleanFact` 容器 |
| `source+0x78` | `Ref_RTTIRefObject`，可能是 debug/info/disable fact 相关 |
| `source+0x80` | `Ref_GraphProgramResource` |
| `source+0x88` | `Ref_SentenceGroupResource`，当前最值得和具体句组关联 |
| `source+0x90/+0x91` | `SentenceGroup` / `DoNotRepeat` bool 类字段 |
| `source+0xA0` | Dollman resource 反射 bool 字段；`0x140C793A0` 默认写 0，字段名仍有解析歧义，不能单独当 Dollman 身份 |

`DSRadioSentenceGroupThroughDollmanResource_Init 0x140C793A0` 静态初始化边界:

- size `0xB0`，vtable 最终写为 `DSRadioSentenceGroupThroughDollmanResource`。
- `+0x64` 默认写 1。
- `+0x68/+0x70/+0x78/+0x80/+0x88` 默认写 0。
- `+0x90` 默认写 0。
- `+0xA0` 默认写 0。
- 因此 runtime 里 `source+0x88` 是否非零、`source+0xA0` 是否被 authored loader 改写，都必须靠实际 probe 采样确认。

`SentenceGroupResource` 字段来自 type descriptor:

| 字段 | 当前判断 |
|---|---|
| `group+0x20` | `ESentenceGroupType` |
| `group+0x28` | `Array_Ref_SentenceResource` count |
| `group+0x30` | `Array_Ref_SentenceResource` item pointer |
| `group+0x38/+0x40` | array capacity / auxiliary storage fields |

`SentenceGroupResource_ExportedGetVoices 0x1402911B0 -> 0x140291960` 静态证明:

- `0x140291960` 读取 `group+0x20` 判断 group type。
- `group+0x28` 作为句子数量读取。
- `group+0x30` 作为 `SentenceResource*` 数组读取。
- 每个 `SentenceResource` 优先取 `+0x58` voice ref，缺失时取 `+0x50` fallback voice ref。
- 这个函数做的是“句组里有哪些 voice resource”的导出/汇总，不直接证明 `controller_index` 是句子数组下标。

下一轮 runtime 要重点看 `source+0x88` 是否非零、`group+0x30` 是否指向稳定句子数组，以及 `voice-queue-identity id=` 是否能作为这个数组的索引或 key。

`VoiceQueueSubmit` catalog key 静态边界:

- `sub_140DA8720(owner+0x38, request_id, &out_index)` 用 `request_id` 做哈希表查找。
- `owner+0x38+0x20` 是 12 字节 bucket 表，bucket 形态为 `{ key, catalog_index, crc/hash }`。
- `owner+0x38+0x2C` 是表容量/掩码来源。
- 命中后 `owner+0x38+0x38[catalog_index]` 返回 catalog entry。
- 因此 `request+0x00` 是 voice catalog key 候选，不是已证明的 `SentenceGroupResource` 数组下标。
- `VoiceQueueSubmit_Candidate` 里的 `r13` 是 `DSSentenceSituationPriorityInfoResource` / voice policy entry 一类的 catalog entry，不是 sentence/resource/event 指针本身。
- `0x140DAD486` 明确读取 `entry+0x24` 并把它用于队列比较/插入排序；`sub_140DAF580` 也用同一偏移和已有队列 entry 比较。
- type dump 把同一类型列为 `DSSentenceSituationPriorityInfoResource`，字段名包括 `SituationHash/Priority/Flag/Flag2/IsDialogue/IsNeedContextCheckDialogue/IsReactionVoice`，但当前静态行为和字段名存在 4 字节解释歧义。
- 因此 probe 采用 offset-first 命名: `u20/u24_sort/u28/u2c/u30/b30/b32/b33/b34`。后续报告可以结合 runtime 值再决定哪些字段是 `SituationHash`、`Priority` 或 flag，不能先把它当内容身份。
- 当前 probe 也补了 catalog entry `+0x40/+0x48` 以及 `q08/q40/q48` 的 vtable 快照；报告脚本会按 `blocked=` 合并 `[voice-queue-identity]` / `[voice-catalog-entry]`，并把 entry qword 与最近的 `SentenceResource` 候选字段做等值提示。

额外静态证据:

- Dollman resource vtable[0] `0x140C72570` 返回 `word_144331370`。
- `VoiceSharedHelper` 会拿传入 source 的 vtable[0] 返回值和 `word_144331370` 比较。
- `0x140C76520` / `0x140C77110` 的 radio update/type-mask 逻辑也把 `word_144331370` 作为一个独立 radio resource 类型分支处理。
- 所以 `self+0x10` 作为 Dollman source resource 的稳定性很强；但它仍然只是资源类型/组身份，不是单句内容身份。

### 2.7 字幕侧当前定位

当前 random gameplay voice 主线不再依赖字幕 mute 成功与否来证明。

字幕侧仍有价值:

- 验证是否确实是 Dollman gameplay 语境
- 收集 `speaker_tag / line_tag / caller_rva / p6 / p7`
- 辅助区分 random、throw/recall、equip、story/private-room

但不能再把 `line_tag` 单独作为最终 voice mute 规则。

已知风险:

- `line_tag=0x1f4` 可出现在无说话人/其他玩家语音附近。
- 某些 random 期间其他字幕消失的问题证明过，宽泛 subtitle 规则很容易误伤。
- `p7_text="Dollman"` 有价值，但用错面或用错生命周期会扩大误伤。

## 3. 当前还没证明的事

这些不能偷当成既定事实:

- 还没拿到 random Dollman 每一句的稳定内容身份。
- 还没证明 `VoiceSharedHelper` 前的哪个字段能稳定映射到 `SentenceResource`、`DSRadioPlayInfo` 或 sound resource。
- 还没证明所有未测到的 random 都一定走同一条 `0x140C74300 -> 0x140C743B0` 链，虽然目前这条链是很强的正向 Dollman gameplay radio classifier。
- 还没证明具体 `VoiceQueueSubmit` request 一定能反推最终 AK event id。
- 还没证明当前只读 probe 长时间游玩完全无性能副作用。

更准确的表述:

- **当前已经证明 random Dollman gameplay voice 有一条更早、更窄的 Dollman-only delay closure 路径。**
- **当前还缺的是内容身份映射，而不是 mute 主路径本身。**
- **静态分析不能把这条路径升级成全集边界；EXE 里仍存在 story/private-room/cutscene/talk surface，资产级引用需要 runtime 样本或资源枚举证明。**

## 4. 下一步打法

### 4.1 主线

继续沿当前已证明链路向上摸:

```text
VoiceSharedHelper
  <- 0x140C743B0 closure
  <- 0x140C7EC50 delay invoke
  <- 0x140C74300 Dollman vtable[8] schedule
  <- DSRadioSentenceGroupThroughDollmanInstance fields / owner resource
```

优先级:

1. 用 read-only probe 确认 `self+0x10/+0x18/+0x20/+0x40/+0x44` 和 source resource 字段。
2. 把稳定字段和 `DS2_V1.7_AUDIO_SUBTITLE_MAP.md` 里的资源布局对齐:
   - `SentenceResource+0x38/+0x48/+0x50/+0x58`
   - `DSRadioSentenceGroupThroughDollmanResource`
   - `DSRadioPlayInfo`
   - Dollman/player `Voices`
   - `ResidentSentenceGroupResource`
3. 如果上游字段仍然不透明，再加 guarded `VoiceQueueSubmit` 诊断。

### 4.2 guarded VoiceQueueSubmit 原则

`0x140DACE30` 很宽，不能常规 blanket 操作。

只允许这样用:

- 只在 Dollman TLS 为真时记录。
- 只做 identity capture。
- 不把它作为 release 默认 mute 点。
- 不扩大到非 Dollman helper caller。
- hook 直接返回成功时必须写 `out_status=0`，避免触发上层 fallback。

成功标准:

- 一条被 mute 的 random Dollman 语音能稳定打印出资源 pointer/id、sentence index、text tag 或 sound id。
- 多次 random 捕获之间能对上同一种身份逻辑。
- Sam/NPC/无名玩家语音不进入同一个 mute 判定。

## 5. 当前工程判断

当前版本相对 v2.1.17 的核心进步:

- 不再靠字幕后一段时间窗口去猜 voice。
- 不再靠宽泛 PostEvent/eventId 兜 random。
- 找到了 random Dollman gameplay voice 的早期 Dollman-only closure。
- 语音 mute 与字幕误伤问题解耦: voice 由 closure TLS 约束，字幕另行处理。

当前缺点/风险:

- 内容身份还没解出。
- 为了研究多了只读日志字段，理论上有轻微日志/读内存开销。
- 旧 StartTalk 链仍保留，后续可能需要精简成 release 形态。

## 6. DEAD_ENDS / 不要再混用的旧结论

### 6.1 旧 v1.6 / v2.1.17 地址

这些旧地址不能再直接用于当前 v1.7 dynamic-resolution 判断:

- `sub_140DAC7B0`
- `sub_140DAC910`
- `sub_140C73EE0`
- `sub_140C73E30`
- `caller_rva=0x385C1B` 作为当前唯一值

当前 v1.7 对应面已经整体漂移，必须使用 live resolve / 当前 IDA 地址。

### 6.2 把 `0x140C73F30` 写成当前 random Dollman 主 closure

错误想法:

- 旧文档里 `0x140C73F30 -> VoiceSharedHelper`，所以它就是当前 random Dollman 主路径。

现结论:

- 当前已证明主路径是 `0x140C74300 -> 0x140C7EC50 -> 0x140C743B0 -> 0x140DACCD0`。
- `0x140C73F30` 只能保留为 sibling/player 对照路径，不能当当前 random Dollman 证据。

### 6.3 blanket mute `VoiceSharedHelper`

错误想法:

- `VoiceSharedHelper` 足够靠后，直接返回成功最省事。

现结论:

- 它是共享 helper。
- 只有带 `0x140C743B0` Dollman closure TLS 约束时才可消费。
- 否则极易误伤 Sam/NPC/player voice。

### 6.4 把 `event=0` 当失败

错误想法:

- `voice-helper event=0`，所以不知道 eventId 就说明没打中。

现结论:

- 当前 mute 在 `VoiceQueueSubmit` 之前发生。
- `event=0` 是早期消费的自然结果。
- 内容身份应从上游对象或 guarded queue submit probe 里找。

### 6.5 把单一 `line_tag` 当最终规则

错误想法:

- 某个 random 见过 `line_tag=0x1f4/0x357d/...`，直接列表匹配即可。

现结论:

- line tag 是运行时资源 tag，不是全局 Dollman 身份。
- 单独使用会误伤无名玩家/Sam/NPC 或导致期间其他字幕消失。
- 它只能作为观测字段或辅助分类。

### 6.6 把 private-room/story 行为混入 gameplay random

错误想法:

- 只要是 Dollman 声音，都走同一套 gameplay random 逻辑。

现结论:

- private-room / story / gameplay random 必须分开验证。
- 当前这份文档讨论的主链是 gameplay random voice。
- 休息室正常与否不能直接证明 random mute 完成，反过来也一样。

## 7. 维护规则

只接受三类内容:

- 当前仍在指导代码和实验的事实
- 当前仍成立的结构判断
- 已经足够明确、值得写进 DEAD_ENDS 的坑

不要再加:

- 单次日志洪水
- 早已被推翻的旧偏移叙事
- “也许以后看”的考古碎片
- 未标明版本/分支/证据来源的结论

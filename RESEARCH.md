# DollmanMute 研究笔记

> 最后更新: 2026-05-25
> 当前源码 build tag: `v2.1.17`
> 当前文件只记录仍会指导代码、验证和调试决策的事实。旧偏移、旧日志洪水、已推翻路线不再作为“当前结论”出现。

## 0. 目标

唯一目标:

- 只屏蔽 Dollman 的 gameplay / random 语音和字幕。
- 尽量不误伤 Sam、NPC、过场、私房间、其他玩家无名语音和普通系统声音。
- 语音和字幕要尽量同步；不能用“字幕没了但声音还在”或“声音没了但其他字幕也消失”当成可交付结果。
- 不能把能启动、能热更、单个场景成功误当成终点；最终需要靠日志链路和长时间游玩样本提高信心。

## 1. 当前产品策略

当前主线已经从早期的 eventId 白名单 / 时间窗口，推进到更接近语义的链路:

```text
StartTalk.GetOrCreateSoundWrapper
  -> 读取 StartTalk line / sound / voice resource
  -> 用 voice resource speaker_tag/text 判断是否 Dollman
  -> 缓存 sound resource / instance identity
  -> SoundInstanceSubmit 绑定 sound instance
  -> PostEventID 只按已绑定 sound instance 的 gameObjectID 阻断
```

这比 release/main 时代的核心进步是:

- 不再靠“Dollman 字幕后 1.5 秒窗口”挡声音，避免 random 期间其他字幕/声音被连带影响。
- 不再把 `line_tag=0x1f4` 之类当身份，因为 line tag 会复用，容易误伤无名玩家或其他短句。
- 不再只靠固定 eventId/ext0 组合覆盖已见过样本；新 line tag 只要走同一 StartTalk speaker 链，就能自动覆盖。
- `PostEventID` 仍是最终阻断点，但阻断依据从“这个 eventId 看起来像 Dollman”变成“这个 PostEvent 的 gameObjectID 来自刚确认过的 Dollman StartTalk sound instance”。

当前 `PostEventID` block mode 首选:

```text
sender-only-starttalk-speaker
```

旧 fallback 仍可能存在:

```text
sender-only-narrow
```

但它应只作为兼容兜底，不再是主解法。看到新日志里频繁命中 `sender-only-narrow` 时，要重新审计是否退回了旧策略。

## 2. 当前默认 hook 面

当前默认应启用:

| Hook 面 | 角色 |
|---|---|
| `StartTalkFunction.GetOrCreateSoundWrapper.sub_140387FB0` | 从 StartTalk 早期 payload 读取 line / sound / voice / speaker |
| `SoundInstanceSubmit.sub_14026B8410` | 把 sound resource / owner 绑定到实际 sound instance |
| `AK::SoundEngine::PostEventID` | 最终阻断已绑定的 Dollman sound instance |
| `GameViewGame.ShowSubtitleSender` | 字幕侧按 sender payload 精准静音 |
| `GameViewGame.RemoveSubtitleSender` | 字幕 remove 配对 |

当前默认不应启用为产品路径:

- voice delay schedule / closure
- voice queue submit
- voice shared helper
- selector / builder / producer 大范围探针
- subtitle runtime legacy wrapper

这些可以作为深度探针使用，但不应该常驻影响游玩验证。现在 x64dbg 也不应常驻断点验证；优先靠日志探针和静态分析。

## 3. 字幕侧事实

当前 gameplay Dollman 字幕最稳判断:

```text
caller_rva  = 0x385c5b
speaker_tag = 0x12b6f
```

已确认:

- `speaker_tag=0x12b6f` 是 Dollman gameplay speaker。
- sender payload 中 `p7` 可解析出 speaker，历史日志能看到 `p7_text="Dollman"`。
- `p6` 可解析 line/body。
- 字幕静音应使用 `caller_rva + speaker_tag` pair。
- `line_tag` 只适合做审计和分类，不适合单独当身份判定。

已经见过并被当前方案覆盖的 line tag 包括:

```text
0x1f4
0x357d
0x3745
0x425a
0x7e4a
0x8026
0x87fb
0x12b6b
```

关键经验:

- `line_tag=0x1f4` 不是 Dollman 专属。
- “看到 Dollman 字幕”不能自动推出“声音也已经被挡”；必须看声音链后续 `Bound -> Blocked`。
- 宽泛 subtitle 时间窗口曾导致 random 期间其他字幕消失，不能恢复成产品策略。

## 4. 声音侧事实

### 4.1 StartTalk 读取点

当前 StartTalk 结构中最关键字段:

```text
StartTalk + 0xC8 -> slot
slot      + 0x00 -> line
line      + 0x38 -> sound resource
line      + 0x48 -> subtitle/body
line      + 0x50 -> voice fallback
line      + 0x58 -> voice preferred
```

voice resource speaker offset 当前扫描:

```text
0x28, 0x30, 0x38, 0x40, 0x48
```

Dollman speaker 判断:

```text
speaker_tag  = 0x12b6f
speaker_text = "偶人"
```

`starttalk_flags == 0` 当前视为可 mute；非 0 flags 当前会走 bypass，用来保护私房间、剧情或其他高风险 Talk。

### 4.2 SoundInstance 绑定点

`SoundInstanceSubmit` 当前用于把 StartTalk 确认过的 sound resource 绑定到真正要提交给 Wwise 的 sound instance:

```text
sound_instance + 0x178 -> p178
p178           + 0x00  -> p178_0
p178_0         + 0x20  -> owner
```

如果 `owner` 或 `p178_0` 命中最近的 Dollman StartTalk sound cache，则记录 `DollmanStartTalkBound`，后续 `PostEventID` 只拦这个 sound instance 对应的 `gameObjectID`。

当前 cache:

```text
DOLLMAN_STARTTALK_CACHE_TTL_MS = 10000
cache slots = 128
```

这解决了高倍速测试时“听不到声音所以无法人工判断”的问题：日志链完整就能判断是否成功。

### 4.3 PostEvent 阻断点

`PostEventID` 不再应该凭宽泛窗口阻断。当前精确条件是:

```text
external_source_count == 1
gameObjectID 命中最近 Dollman StartTalk sound instance
该 instance 没有被 flags/bypass 标记排除
```

成功时日志应看到:

```text
DollmanStartTalkCandidate mute=1 ...
DollmanStartTalkBound mute=1 ...
Blocked PostEventID mode=sender-only-starttalk-speaker ...
```

如果只看到 `DollmanStartTalkCandidate`，但没有后续 `Bound` 或 `Blocked`，那是潜在漏音样本。

如果看到:

```text
DollmanStartTalkBound mute=0
BypassStartTalkSpeakerPostEvent
```

通常说明 StartTalk flags 非 0，被当前保护逻辑放行。此时要结合场景判断是正确保护，还是误放。

## 5. 当前验证口径

在高倍速游玩时，不靠耳朵判断，靠日志链:

### 成功 mute

```text
DollmanStartTalkCandidate mute=1 ... speaker_tag=0x12b6f ... body_text="..."
Muted subtitle surface=sender ... speaker_tag=0x12b6f ...
DollmanStartTalkBound mute=1 eventId=... soundInstance=... deltaMs=...
Blocked PostEventID mode=sender-only-starttalk-speaker eventId=...
```

看到这条链，基本可以认为该句 Dollman 字幕和声音都被当前规则覆盖。

### 潜在漏音

```text
DollmanStartTalkCandidate mute=1 ...
```

但没有:

```text
DollmanStartTalkBound mute=1
Blocked PostEventID mode=sender-only-starttalk-speaker
```

这说明 StartTalk 识别到了 Dollman，但后续 sound instance / PostEvent 绑定没跟上。

### 潜在误伤

需要重点查:

```text
Blocked PostEventID mode=sender-only-starttalk-speaker
```

附近是否没有 Dollman candidate，或 candidate 的 `mute=0` 却仍被挡。

也要查 Sam/NPC/private-room/查看 BB/无名玩家语音场景是否出现非 Dollman 的 `Blocked PostEventID`。

## 6. 已验证过的高风险场景

近期重点验证过:

- 丢出 / 收回 Dollman。
- 戴帽子 Dollman 语音。
- 狙击枪相关 Dollman 语音。
- 货物掉落相关 Dollman 语音。
- random gameplay 剧情回顾。
- 私房间 / 休息室 Dollman 对话保护。
- Sam 打招呼。
- 查看 BB 时 Sam 说话。
- 无名玩家短句，例如 `Just little ol' me.`、`Helloooo!!`。

当前结论:

- 字幕侧基本是按 Dollman speaker 身份挡。
- 声音侧已经不是单纯 eventId 白名单，而是 StartTalk speaker -> sound instance -> PostEvent 的对象链。
- 当前比 release/main 更稳，尤其是 random 期间误伤其他字幕的问题已经被旧时间窗口方案替代掉。

## 7. 当前剩余风险

还不能偷换成“100% 全游戏已证明”的点:

- 某些 Dollman 声音可能不走当前 StartTalk.GetOrCreateSoundWrapper 链。
- voice resource 的 speaker offset 未来版本可能漂移，或存在未扫描到的新布局。
- `starttalk_flags != 0` 的 Talk 当前会保护放行；如果其中有用户期望屏蔽的 gameplay Dollman，就会漏。
- SoundInstance 到 PostEvent 的绑定可能超出 10 秒 TTL 或 cache 被极端高频样本挤掉。
- 当前只针对 DS2 v1.7 这一轮样本验证充分；游戏后续更新仍要重新验证关键 RVA / 字段。
- 全流程自然游玩还没有覆盖所有章节、任务、地图和装备状态。

当前信心:

- 已见过的 Dollman gameplay/random 样本：较高。
- 字幕身份过滤：较高。
- 声音对未知 Dollman 语音的自动覆盖：比 release/main 高，但不是数学证明。
- 完整全游戏零漏音、零误伤：仍需要继续靠长时间样本和日志审计。

## 8. x64dbg 当前策略

当前不把 x64dbg 大范围断点当常规工作流。

原因:

- 用户侧已经观察到断点可能导致游戏崩溃或冻结。
- 热更新/attach 过程中可能自动出现 TLS 断点，影响验证节奏。
- 当前日志链已经能回答大多数“是否挡住”的问题。

默认策略:

- x64dbg 保持 attached / running 即可。
- breakpoints 目标状态为 0。
- 如果热更新或 attach 后出现自动 TLS 断点，删除后继续运行。
- 只有日志链无法解释的新问题，才考虑短时间、单点、可恢复断点。

优先工具顺序:

1. 运行时日志链。
2. 静态分析 / IDA。
3. 只读探针。
4. 最后才是 x64dbg 断点。

## 9. 热更新和日志 UX

当前热更新 MessageBox 已改成可选:

```text
DOLLMANMUTE_SHOW_RELOAD_MESSAGE=1
```

只有设置该环境变量时才弹窗。默认不弹，避免影响用户游玩。

日志中最有价值的关键字:

```text
DollmanMute build
DollmanStartTalkCandidate
Muted subtitle
DollmanStartTalkBound
StartTalkSpeakerPostEvent
BypassStartTalkSpeakerPostEvent
Blocked PostEventID
sender-only-starttalk-speaker
sender-only-narrow
```

建议筛选命令:

```powershell
Select-String -Path "C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log" -Pattern "DollmanStartTalkCandidate|DollmanStartTalkBound|Muted subtitle|Blocked PostEventID|sender-only-narrow|sender-only-starttalk-speaker|BypassStartTalkSpeakerPostEvent|DollmanMute init complete|core_shutdown" | Select-Object -Last 120
```

## 10. 旧路线和死路

### 10.1 时间窗口策略

错误:

- Dollman 字幕出现后，在一个时间窗口内屏蔽声音或字幕。

问题:

- random 期间其他字幕曾经消失。
- 可能挡到 Sam/NPC/无名玩家。

当前结论:

- 不能作为产品策略。
- 只能作为历史问题解释，不应恢复。

### 10.2 line_tag 身份策略

错误:

- 用 `line_tag=0x1f4` 或其他 line tag 直接判断 Dollman。

问题:

- line tag 会跨不同 speaker / 无名玩家 / 短句复用。
- 这解释了之前“为什么明明能拿到 Dollman 文本还会误伤”的一部分问题。

当前结论:

- line tag 只用于审计、分类、样本记录。
- 身份判断必须用 speaker/resource/object 链。

### 10.3 纯 eventId/ext0 白名单

错误:

- 看到一个 Dollman eventId/ext0 就补一个规则。

问题:

- 会变成“遇到一个修一个”。
- 对未知 random 台词覆盖差。

当前结论:

- 可以保留极窄 fallback，但不能作为主解法。
- 当前主解法是 StartTalk speaker -> SoundInstance -> PostEvent。

### 10.4 voice closure / shared helper 主线

历史判断:

- `sub_140C73EE0` / `sub_140DAC7B0` / queue submit 可能是最终切点。

后续 live 证据:

- 它们对理解 voice 系统有价值。
- 但 throw/recall 残余声音曾经不稳定进入这些 probe 面。
- `sub_140DAC7B0` 是 Player / Dollman 共用 helper，blanket mute 风险高。

当前结论:

- 不作为产品常驻 mute 点。
- 只作为深度研究或结构理解的辅助路线。

### 10.5 ShowSubtitle 末端万能论

错误:

- 字幕 sender 能精准命中，所以把所有问题都堆到字幕末端解决。

问题:

- 字幕和声音最后不是同一个出口。
- 字幕成功不代表声音成功。

当前结论:

- 字幕 sender 是强验证面和字幕产品面。
- 声音必须有自己的 StartTalk/SoundInstance/PostEvent 链。

## 11. 下一步

优先做三件事:

1. 继续用真实游玩日志收集 `Candidate -> Bound -> Blocked` 完整链，尤其是没有测到的新 random gameplay 台词。
2. 对所有 `Candidate mute=1` 但没有 `Bound/Blocked` 的样本建单独清单。
3. 对所有 Sam/NPC/private-room/无名玩家附近的 `Blocked PostEventID` 做误伤审计。

如果继续做静态分析，重点不是再找更多 eventId，而是继续确认:

- StartTalk line / voice resource 布局是否还有其他 speaker 字段。
- `starttalk_flags` 的语义，哪些 flags 应该保护，哪些 flags 仍应 mute。
- SoundInstance owner 链是否存在更稳定的 source/speaker 对象。
- DS2 小更新后 `0x385c5b`、StartTalk wrapper、SoundInstanceSubmit、PostEvent caller 是否漂移。

## 12. 维护原则

以后这份文件只接受:

- 当前仍在指导代码和实验的事实。
- 当前仍成立的结构判断。
- 已经足够明确、值得写进死路的坑。
- 能帮助用户判断“这版是否更好、哪里还有风险”的验证标准。

不要再加入:

- 单次临时日志洪水。
- 早已被推翻的旧偏移叙事。
- 只对某一轮调试过程有意义的长篇过程记录。
- 没有证据闭环的“也许 / 可能 / 以后再看”。

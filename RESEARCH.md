# DollmanMute 研究笔记

> 最后更新: 2026-04-26
> 当前源码 build tag: `research-v3.27f-postevent-narrow-block`
> 这份文件只保留当前仍有价值的事实、思路、速查和死路。
> 更老的探针史、旧偏移试错、长篇考古，统一留给 git 历史，不再堆在这里。

## 0. 目标

唯一目标一直没变:

- **只屏蔽 Dollman 的 gameplay 语音和字幕**
- **尽量不误伤 Sam / NPC / cutscene / 其他系统**
- **不要把“游戏能启动”误当成终点**

平台稳定、热更新、日志 UX 都只是推进这个目标的手段。

## 1. 当前已确认事实

### 1.1 当前默认运行时形态

当前源码默认值来自 `src/core_main.c`:

```ini
[General]
Enabled=1
VerboseLog=0
EnableDollmanRadioMute=1
EnableThrowRecallSubtitleMute=0
ActiveSubtitleStrategy=1
EnableSenderOnlyRuntimeMode=1
EnableSubtitleRuntimeHooks=1
EnableLegacyRuntimeWrapper=0
EnableSubtitleFamilyTracking=0
EnableSubtitleProducerProbe=0
EnableBuilderProbe=0
EnableSelectorProbe=1
EnableDeepProbe=0
EnableTalkDispatcherProbe=0
ScannerMode=0
```

这意味着当前默认口径是:

- sender-only runtime mode 开
- legacy subtitle runtime wrapper 关
- legacy broad Dollman audio path 关
- subtitle sender/remove surface 开
- selector probe 开
- deep probe 关
- TalkDispatcher probe 关

### 1.2 当前默认真正常驻的 hook 面

从当前源码和最新 `DollmanMute.log` 看，默认常驻面是:

| 面 | 当前入口 | 角色 |
|---|---|---|
| voice shared helper | `sub_140DAC7B0` | Player / Dollman 共用的语音提交 helper |
| Dollman voice closure | `sub_140C73EE0` | 当前最窄的 Dollman-only 语音切点 |
| subtitle sender | `sub_140780740` | 当前最稳的 gameplay 字幕静音面 |
| subtitle remove sender | `sub_140780840` | 和 sender 配对，用于 key 对齐 |
| selector dispatch | `sub_140DAF8A0` | 当前最干净的 gameplay 观测锚点 |

默认**不会**常驻的旧路径:

- `PostEventID` broad legacy 音频截断
- `sub_140780690` legacy runtime wrapper
- `TalkDispatcher`
- producer / builder / StartTalk init 深 probe

### 1.3 当前 subtitle 侧最硬的 live 结论

- 当前 gameplay Dollman sender 精确 pair 已坐实:
  - `caller_rva = 0x385C1B`
  - `speaker_tag = 0x12B6F`
- 当前 remove 配对 caller 是:
  - `caller_rva = 0x385B32`
- 当前 `LocalizedTextResource` vtable RVA 是:
  - `0x3448D38`
  - 旧 `0x3448E48` 已失效
- sender 路径当前已经能稳定直接解出 `p6 / p7`
  - `p6` = line/body
  - `p7` = speaker
- 当前 line identity tag 至少已知:
  - `0x01F4` = throw / recall
  - `0x222C` / `0x4377` = dialogue

最新日志里已经出现过这类命中:

```text
SubtitleHit surface=sender caller_rva=0x385c1b speaker_tag=0x12b6f line_tag=0x1f4 family=throwRecall
Muted subtitle surface=sender strategy=pair caller_rva=0x385c1b speaker_tag=0x12b6f line_tag=0x1f4 family=throwRecall
```

这说明 subtitle 侧已经不是“盲炸 UI 尾部”，而是能在 sender 面做当前 build 对齐后的精准命中。

### 1.4 当前 voice 侧最硬的静态结论

当前更可信的 Dollman 语音链应记成:

```text
Dollman:
  sub_140C73E30
    -> sub_140C7E780
    -> sub_140C73EE0
    -> sub_140DAC7B0

Player:
  sub_140C739B0
    -> sub_140C7E760
    -> sub_140C73A60
    -> sub_140DAC7B0
```

结论:

- `sub_140DAC7B0` 是 **Player / Dollman 共用 helper**
- `sub_140C73EE0` 才是当前更窄的 Dollman-only closure 切点
- blanket 拦 `sub_140DAC7B0` 风险很高
- 当前 voice 路和 subtitle 路已经证明属于同一套 talk/controller 生态，但**还没有**证明它们共享同一个最终 identity 字段

### 1.5 当前 throw/recall 残余语音的 live 结论

- 当前 build 下，throw/recall 残余语音已经通过 `PostEventID / Wwise` 被 live 坐实
- 最关键的两个 eventId:
  - `2820786646` = throw
  - `2978848044` = recall
- 它们在 `F8` 窗口里会和 `SubtitleHit caller_rva=0x385C1B speaker_tag=0x12B6F line_tag=0x1F4` 紧邻出现
- 当前抓到的 `PostEventID` caller RVA 统一落在:
  - `0x026B6846`
- 这个 caller 本身区分度不高；真正有价值的是 **eventId**
- 因此对当前残余 throw/recall 语音，最稳的工程切点已经不是 `DAC7B0 / DAC910`，而是这两个 Wwise eventId
- 当前源码已经切到 sender-only 下的窄拦截版本：
  - `research-v3.27f-postevent-narrow-block`
  - sender-only 模式下只拦 `throw / recall` 这两个 eventId
  - 同时保留 `F8` 窗口里的 `[postevent]` 观测

### 1.6 当前更高层的语义锚点

我们已经不只是 stuck 在 UI / payload 尾部。

当前 gameplay 主链上层已经摸到:

- `DSElevenMonthBBControllerComponent`
- `DSElevenMonthBBControllerComponentResource`
- `EDSElevenMonthBBReactionEvent`
- `TargetEvent`
- `ForceToOverride`
- `loop_time`
- `RUNNING_REACTION_EVENT_ST / LP / ED`
- 一组 `EVENT_BB_*` reaction 名

当前最值得记住的链不是旧偏移名，而是:

```text
gameplay action
  -> sub_140F04490
  -> sub_140F01F90
  -> sub_140F000B0
  -> sub_140DAF8A0
  -> builder / StartTalk / subtitle / voice 分叉
```

这条链的意义是:

- 我们已经摸到 reaction / family / selector 语义层
- 不应再把主线重新缩回 `ShowSubtitle` 尾部或 `DSTalkManager` 旁路

## 2. 当前真正可用的工程判断

### 2.1 subtitle 侧

当前 subtitle 路最合理的工程策略是:

- 以 `ShowSubtitle sender` 为主静音面
- 以 `caller_rva + speaker_tag` pair 做精确主判定
- 以 `line identity tag` 做 family 分类补充
- 把 `remove sender` 当配对面，而不是另起一套过滤逻辑

为什么:

- sender 面 blast radius 最小
- 当前 build 上它已经能直接读到 `p6/p7`
- family 可以直接从当前 payload 的 line tag 推出，不再强依赖旧 producer TLS

### 2.2 voice 侧

当前 voice 路最合理的工程策略是:

- 优先从 `sub_140C73EE0` 这种窄 Dollman closure 下刀
- 把 `sub_140DAC7B0` 当共享 helper 看待，只能带强约束地用
- 不要再把旧 broad legacy 音频路径当最终答案

为什么:

- `sub_140DAC7B0` 已坐实 Player / Dollman 共用
- 共享 helper 更适合作观测或二级约束，不适合无脑 mute
- 语音残余很可能不是“字幕那条 sender 路自动顺手解决”的副产品

### 2.3 当前最合理的主线

最大的工程突破点不是“再加更深的末端 hook”，而是把 mute 逻辑重新围绕更高层的**语义阀门**组织:

- reaction event
- family
- selector
- builder 输入语义
- StartTalk 早期字段

字幕 sender 面现在更像:

- 已经验证过的现役出口
- 当前 live 样本回收面
- 用来判断上游探索是否正确的闭环面

而不是最终该把所有逻辑都堆上的地方。

## 3. 当前还没证明的事

下面这些点不能偷着当成既定事实:

- 还没找到一个已实锤的“语音 + 字幕共用最终统一入口”
- 还没证明 `StartTalkFunction` 里的那组 identity / key / mode 字段，原样直通到 `sub_140DAC7B0`
- 还没证明 throw / recall 的所有残余语音都一定走当前已知 Dollman closure
- 还没证明 `Speaker` observer 旁路已经完全排除
- 还没证明“当前 subtitle pair 成功”就意味着整体 mute 方案已经可交付

更准确的表述是:

- **我们已经证明 voice 和 subtitle 挂在同一套 talk/controller 生态上**
- **但还没有证明它们在末端仍共享同一份判别字段**

## 4. 当前下一步应该怎么打

### 4.1 上游优先

继续往上摸，不要退回末端迷宫。

优先级顺序:

1. `DSElevenMonthBBControllerComponent`
2. `EDSElevenMonthBBReactionEvent`
3. `sub_140F04490 -> sub_140F000B0 -> sub_140DAF8A0`
4. `StartTalk` 早期字段与 builder 输入
5. `voice closure / shared helper`
6. `ShowSubtitle sender` 作为验证闭环

### 4.2 subtitle 的角色

subtitle 侧现在的职责是:

- 做 live 命中验证
- 回收 `speaker / line / caller / family` 样本
- 给上游语义判断提供“是否真的打中 Dollman gameplay”的现实校验

而不是把所有问题都留到 UI 尾部解决。

### 4.3 voice 的角色

voice 侧当前更像主阻塞项。

下一轮若继续研究，应优先回答:

1. `sub_140C73EE0` 已经足够窄了吗
2. throw / recall 残余语音是否经 `sub_140DAC7B0` 之外的旁路提交
3. 哪一层最早把 Dollman 和 Player 语义真正分叉开

## 5. 当前运行时与工具速查

### 5.1 热键

- `J` = throw/recall mute ON
- `K` = throw/recall mute OFF
- `N` = dialogue mute ON
- `M` = dialogue mute OFF
- `F12` = clear all runtime subtitle mutes
- `F1..F6` = 切字幕策略
- `F8` = 打 session 边界并开 5 秒 probe window
- `F9` = 清空 `DollmanMute.log`

### 5.2 当前 subtitle 策略

- `F1 = observe`
- `F2 = pair`
- `F3 = callerOnly`
- `F4 = speakerOnly`
- `F5 = selectedFamily`
- `F6 = pairOrSelectedFamily`

当前默认主策略仍是:

- `pair`
- 也就是 `caller_rva=0x385C1B + speaker_tag=0x12B6F`

### 5.3 当前最顺手的探索命令

```powershell
.\tools\exp.ps1 sessions
.\tools\exp.ps1 summary F8-<N> -Top 4
.\tools\exp.ps1 show F8-<N> -Samples 3
.\tools\exp.ps1 combo F8-<N> 3 4
.\tools\exp.ps1 watch
```

建议:

- 先 `F9`
- 再 `F8`
- 做一组单一动作
- 最后用明确的 `F8-N` 去看，而不是偷懒盯“last”

## 6. DEAD_ENDS

下面这些坑已经足够明确，不要再反复掉进去。

### 6.1 把旧偏移当真实函数入口

错误想法:

- `0x00DAFAD0`
- `0x003857E0`
- `0x00780710`

现结论:

- 它们最多只是历史 probe 点或父函数内部偏移
- 当前讨论必须尽量落在真实函数起点和现 build 对齐后的语义面上

### 6.2 把 `0x3448E48` 当当前 `LocalizedTextResource` vtable

错误想法:

- sender 路径解不出 `p6/p7`，所以这条路没用

现结论:

- 当前 live vtable 是 `0x3448D38`
- 旧常量漂移才是之前 `speaker_ok=0 / line_ok=0` 的原因

### 6.3 把 `q2/q3` 当 Dollman 身份 ID

错误想法:

- `q2/q3` 看起来稳定，所以可直接拿来做 Dollman 过滤

现结论:

- `q2/q3` 更像通用 chatter sentinel
- 用它做过滤会把 Sam / NPC / 非目标字幕一起炸掉

### 6.4 把 `sub_140385720` / `sub_1403857E0` 当“音频字幕统一最终入口”

错误想法:

- 找到一个统一 dispatcher，直接一刀切就完了

现结论:

- 它更像 gameplay 字幕 payload packer / broadcaster 历史面
- 这里已经太靠 subtitle 侧，不足以代表整个 voice + subtitle 统一终点

### 6.5 把 `DSDollmanTalkManager` 当 gameplay chatter 主轴

错误想法:

- 名字写着 Dollman，所以主链一定在这

现结论:

- 它更像 story-demo / wakeup / private-room / lifecycle 混合系统
- 误伤风险高，解释 gameplay chatter 的能力反而弱

### 6.6 把 broad 策略当产品策略

错误想法:

- `callerOnly`
- `speakerOnly`
- legacy broad audio mute

现结论:

- 它们可以做实验和兜底
- 不能当最终交付策略
- 一旦当最终策略，Sam / NPC / player 行为很容易被一起带死

### 6.7 在当前 build 上重开 TalkDispatcher 主探针

错误想法:

- 既然它历史上重要，就该常驻

现结论:

- 当前 build 上它仍然是隔离项
- `EnableTalkDispatcherProbe=0` 不是保守，是明确的风险控制

### 6.8 把 `sub_140DAC7B0` 当可随便 blanket mute 的点

错误想法:

- 共享 helper 足够靠后，拦这里最省事

现结论:

- 这是 Player / Dollman 共用 helper
- 任何不带强约束的 blanket mute 都极易误伤 player 语音链

### 6.9 把 `ShowSubtitle` 末端继续当唯一主战场

错误想法:

- 只要 sender pair 已经能打中，就继续把所有逻辑往下堆

现结论:

- sender 面现在很有价值，但它更适合做验证闭环
- 真正该继续突破的是 reaction / family / selector / StartTalk 早期语义

### 6.10 把 x64dbg 大范围断点当常规工作流

错误想法:

- 断点越多越接近真相

现结论:

- 这会严重破坏 live 体验和节奏
- 当前更好的默认工作流是:
  - 静态分析
  - sender-only runtime 日志
  - F8 session
  - `tools/exp.ps1`

### 6.11 把 `0x140DAA410` 当成 throw/recall 语音的最终语义入口

错误想法:

- `sub_140DAA410` 既然能持续打出 `[voice-dispatch]`，那它就是最该拦的 live dispatcher
- 日志里长期出现的 `mode=-1 / key=0xC0176000 / param=0` 可以直接拿来定性业务字段

现结论:

- `sub_140DAA410` 的真实语义更像 `DollmanVoiceDispatcher(state, dt)`：
  - 唯一静态 caller 在 `sub_140DBCDE0`
  - 调用前准备的是 `RCX=state`、`XMM1=dt`
  - IDA 误判出来的“第二个整数参数”不可靠
- `sub_140DA8D40` 已经把 `sub_140DAC910` request 布局静态坐实:
  - `+0x00 key`
  - `+0x08 ref/speaker`
  - `+0x10 index`
  - `+0x14 flag0`
  - `+0x18 raw32_a`
  - `+0x1C raw32_b`
  - `+0x20/+0x21/+0x22 flag1/2/3`
  - `+0x24 param`
- 因此旧日志里的 `key=0xC0176000` 更像字段映射错位，不能再当真实 talk key 继续推理
- 当前更有价值的 live 入口是:
  - `sub_140DAC7B0`：Player / Dollman 共享 helper
  - `sub_140DAC910`：更早的 queue submit 面，能直接看到 raw request
- `sub_140DAC7B0` 这层如果要区分 `PlayerInstance` / `DollmanInstance`，当前最稳的信号不是 request 字段，而是 helper 自己的 caller return address:
  - `0x00C73AEE`：Player 路
  - `0x00C73F6D`：Dollman 路
- `sub_140DAC910` 这一层当前最值得盯的 caller return address:
  - `0x00DAC891`：来自 `sub_140DAC7B0`
  - `0x00DAAB64`：`sub_140DAA410` 的 synthetic request
  - `0x00DAC17A`：`sub_140DAA410` 的 per-entry forward
- 这也解释了为什么 `research-v3.27d-voice-queue-retaddr` 热更后如果没有新的 `F8` session，就不会出现新的 `[voice-shared]` / `[voice-queue]` 样本：
  - 不是 hook 失效
  - 而是 probe window 根本没打开
- 下一轮 live 默认动作:
  - 保持 sender-only 字幕面不动
  - 在 `F8` 窗口里同时看 `[voice-shared]` 和更早的 `DAC910` raw request
  - 先判清 `throw/recall` 语音到底是 `shared-helper` caller，还是被 `DAA410` 这一层再包装/转发

### 6.12 在当前 build 上继续把 throw/recall 残余语音的主线压在 `sub_140C73EE0 -> sub_140DAC7B0 -> sub_140DAC910 / sub_140DAA410`

错误想法:

- 既然 subtitle 命中和 Dollman gameplay 已经对上，那残余 throw/recall 语音大概率也还在这条已知 talk/queue 链里
- 只要继续深挖 `C73EE0 / DAC7B0 / DAC910 / DAA410`，总会在这条链上找到最后那一刀

现结论:

- 这条线对“当前 build、当前残余 throw/recall 语音”已经足够明确地**降格成 DEAD_ENDS**
- 证据不是静态猜测，而是 live 否定:
  - `F8` 窗口里反复出现 `SubtitleHit / Muted subtitle`
  - 同一窗口里是 **0 条** `[voice-shared]`
  - **0 条** `[voice-queue]`
  - **0 条** `Muted Dollman voice closure`
- 这意味着当前残余语音至少**不是**下面这些已知路径:
  - `sub_140C73EE0` 的 Dollman closure
  - `sub_140DAC7B0` 的 shared helper
  - `sub_140DAC910` 的 queue submit
  - `sub_140DAA410` 包装/转发进去的那两类 request
- 因此后续不该再把这条链当 throw/recall 残余语音的第一主线
- 更准确的保留表述是:
  - 这条链对**语音系统结构理解**仍然有价值
  - 但对**当前残余 throw/recall 语音的直接突破**，优先级已经明显低于更偏 Wwise / gameplay audio emitter / 直发事件 的路线
- 以后如果再回到这条线，只能有两个前提:
  - 出现新的 live 证据，证明残余语音重新进入了 `[voice-*]` 面
  - 或者研究目标改成“理解 talk/queue 结构”，而不是“解决当前残余 throw/recall 语音”

## 7. 这份文件以后怎么维护

只接受三类内容:

- 当前仍在指导代码和实验的事实
- 当前仍成立的结构判断
- 已经足够明确、值得写进 `DEAD_ENDS` 的坑

不要再往回加这些东西:

- 单次临时日志洪水
- 早已被推翻的旧偏移叙事
- 只对某一轮调试过程有意义的长篇过程记录
- “也许 / 可能 / 以后再看”的考古碎片

旧研究史如果真有必要回看，直接查 git。

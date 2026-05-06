# DollmanMute 研究笔记

> 最后更新: 2026-05-07
> 当前源码 build tag: `v2.1.5-dev`
> 这份文件只记录对终极目标有帮助的事实:找到并利用 Dollman gameplay 字幕与语音的共同身份/上游。

## 0. 终极目标

- 只屏蔽 Dollman 的 gameplay 语音和字幕。
- 不误伤 Sam / NPC / cutscene / private-room / story 系统。
- 不把末端样本签名误当成稳定语义。
- 最终希望从“字幕一套规则、语音一套规则”推进到“同一类 Dollman gameplay 身份在两侧的可验证投影”。

当前最准确的状态:

- native 层还没有找到一个能同时直接管住字幕和语音的单一函数。
- 字幕和语音更像被更高层 script/dialog 系统分别触发。
- 因此当前主线不是继续堆末端 hook，而是把两侧都提升到身份相关层，再做相关拦截。

## 1. 当前最重要结论

### 1.1 字幕侧上游已经比较清楚

字幕侧 reaction 链:

```text
Invoke_ElevenMonthBBReaction
  -> DSElevenMonthBBControllerComponent reaction state
  -> controller per-frame process
  -> MsgDSStartTalk
  -> ShowSubtitle sender
```

关键事实:

- `sub_140F03A10` 是 `DSElevenMonthBBControllerComponent::Invoke_ElevenMonthBBReaction(reaction_id, force_to_override, loop_time)`。
- 它写 controller component:
  - `+0x508` = force flag
  - `+0x534` = reaction id
  - `+0x540` = loop time
- `sub_140F00D10` 消费这套 reaction state，并多次调用 `sub_140DB05F0` 派发 `MsgDSStartTalk`。
- 这条链解释了 gameplay Dollman 字幕为什么会落到 `ShowSubtitle sender`。

当前字幕命中面:

- `sub_140780BF0` = subtitle sender
- `sub_140780CF0` = subtitle remove sender
- Dollman gameplay sender pair:
  - `caller_rva = 0x385C5B`
  - `speaker_tag = 0x12B6F`
- 已知 line/family:
  - `line_tag = 0x01F4` = throw/recall/equip family
  - `line_tag = 0x222C / 0x4377` = gameplay dialogue samples

工程判断:

- `ShowSubtitle sender` 现在是可靠验证面，但不是最终上游。
- `speaker_tag / line_tag` 是字幕侧可见身份投影，不应直接当成 voice 侧身份。

### 1.2 语音侧不是字幕链的直接下游

已经静态排除的错误假设:

- `Invoke_ElevenMonthBBReaction` 不直接触发 voice。
- `MsgDSStartTalk` 不等于 voice + subtitle 双发统一入口。
- `sub_140DACCD0` 是 Player / Dollman 共用 helper，不能 blanket mute。

当前 v1.6 voice 结构:

```text
Dollman:
  sub_140C74300
    -> sub_140C7E780
    -> sub_140C743B0
    -> sub_140DACCD0

Player:
  sub_140C73E80
    -> sub_140C7E760
    -> sub_140C73F30
    -> sub_140DACCD0
```

关键事实:

- `sub_140C74300` 是当前 Dollman voice delay schedule seam。
- `sub_140C743B0` 是 Dollman delay closure，能兜底但偏晚。
- `sub_140DACCD0` 是 Player / Dollman 共用 shared helper。
- `sub_140DAA930` 是 voice manager tick / emitter 入口。
- `sub_140D8F180` 当前是最有价值的 voice-entry 观测点。

工程判断:

- 如果只在 closure 层 mute，会天然晚一拍。
- 如果只靠 Wwise `eventId/ext0`，会缺少 Dollman 身份约束。
- 当前语音侧最有希望的方向是 voice-entry 身份 -> PostEventID 相关拦截。

## 2. 当前“二合一”实验形态

这里的“二合一”不是已经找到单一 native 入口，而是两侧都开始围绕 Dollman gameplay 身份做判断。

当前链路:

```text
sub_140DAA930 VoiceManagerTick
  -> TLS 记录当前 voice manager

sub_140D8F180 VoiceEntryConsumed
  -> 识别 Dollman gameplay chatter entry
  -> 记录 key/index/selected_index/时间戳

AK::SoundEngine::PostEvent
  -> 若 250ms 内 eventId/ext0 匹配 pending voice-entry
  -> Blocked PostEventID mode=sender-only-voice-entry
```

当前已编码的 voice-entry 样本族:

- `eventId = 4251155871`
- `ext0 = 0x4fd637d9f`
- `(key,index) = (0x341e4861,124)`
- `(key,index) = (0x4b996a15,125)`
- `(key,index) = (0x13cb325c,126)`
- entry 形状:
  - `flags20_22 = 0x100`
  - `param = -1`
  - `active = 1`
- 资源形状约束:
  - `DSPlayerSentenceResource`
  - `SentenceResource`
  - `SentencePriorityInfo`
  - `NotificationQueue`

这条线对终极目标的意义:

- 比裸 `eventId/ext0` 更窄，因为必须先命中 voice-entry 身份。
- 比 subtitle pair 更靠近实际发声路径，因为不要求字幕先出现。
- 给了一个可以和 subtitle sender 同窗验证的 voice 侧身份投影。

仍未证明:

- 这组 `(key,index)` 是否覆盖所有 Dollman gameplay chatter。
- 它是否绝不会出现在非 Dollman / private-room / cutscene。
- 它能否反推回 StartTalk / ref-pack / selector 层的同一语义身份。

## 3. 当前最值得继续向上找的身份

最值得继续验证的不是新的 Wwise eventId，而是 StartTalk 早期复合身份。

当前最强候选:

```text
StartTalk object
  -> vtable+0x40 returns (qword, qword)
  -> sub_140385720 persists 20-byte entry
  -> compare/dedupe uses first two qwords
  -> sub_140385A70 advances downstream broadcast
```

为什么它重要:

- 它被持久保存。
- 它被重复比较。
- 它参与是否推进下游。
- 它比 `speaker_tag / line_tag / eventId / ext0` 更像 selector/speaker compound identity。

第二层稳定容器:

```text
StartTalk object +0xE8..+0x100
  -> 4-slot ref pack
```

已知来源:

- `sub_140387670` 从 `this+0xC8 / this+0xD8` 上游拉出 4 个 ref。
- `sub_1403878E0(this+0xE8, &tmp4refs)` 持久保存这 4 槽。
- `sub_1403881B0` 后续消费这 4 槽，并决定是否走 `sub_140385720 / sub_140385A70`。

当前优先盯:

- `pack[3]`: 来自 `this+0xD8` 虚表 `+8`
- `pack[2]`: 来自 `**(this+0xC8) + 72`
- `vtable+0x40` 返回的 `(qword,qword)`

voice 侧可能对应投影:

- `captured_id`: 更像路由主键。
- `source+0x20`: 更像语义资源主键。
- `sub_140D8F180` 的 `key/index/resource shape`: 当前最接近 voice 侧 live 身份投影。

下一步要证明的是:

- 同一轮 F8 中，StartTalk/ref-pack 身份是否与 subtitle sender Dollman pair 同窗出现。
- 同一轮 F8 中，同一身份是否随后对应 voice-entry / PostEventID。
- 非 Dollman chatter 是否不满足这组身份。

## 4. 当前运行时速查

默认公开配置:

```ini
[General]
Enabled=1
VerboseLog=0
EnableVoiceMute=1
EnableSubtitleMute=1
ScannerMode=0
```

当前关键 hook 面:

| 面 | 入口 | 目的 |
|---|---|---|
| PostEventID | `AK::SoundEngine::PostEvent` | 最终 Wwise gate / F8 audio observation |
| voice manager tick | `sub_140DAA930` | 给 voice-entry 提供 manager 上下文 |
| voice entry consumed | `sub_140D8F180` | 当前 voice 身份主线 |
| Dollman voice schedule | `sub_140C74300` | Dollman voice 早拦截 seam |
| Dollman voice closure | `sub_140C743B0` | 兜底后截 |
| shared helper | `sub_140DACCD0` | Player / Dollman 共用 helper，只能强约束使用 |
| subtitle sender | `sub_140780BF0` | subtitle 侧验证/静音面 |
| subtitle remove sender | `sub_140780CF0` | sender 配对面 |

热键:

- `F8` = 打 session 边界并打开短 probe window。
- `F9` = 清空 `DollmanMute.log`。

最有用的本地命令:

```powershell
.\tools\exp.ps1 sessions
.\tools\exp.ps1 summary F8-<N> -Top 4
.\tools\exp.ps1 show F8-<N> -Samples 3
.\tools\exp.ps1 combo F8-<N> 3 4
.\tools\exp.ps1 watch
```

## 5. 当前不可再浪费时间的路线

- 不要把 broad mute 当产品策略。
- 不要把 `ShowSubtitle` 末端当唯一主战场。
- 不要把 `sub_140DACCD0` 当可 blanket mute 的 Dollman 点。
- 不要把 `eventId/ext0` 单独当 Dollman 身份。
- 不要把 `q2/q3` 当 Dollman 身份 ID；它更像通用 chatter sentinel。
- 不要把 `DSDollmanTalkManager` 当 gameplay chatter 主轴；它更像 story/private-room/lifecycle 混合系统。
- 不要用 x64dbg 大范围断点当常规工作流；除非已经有窄断点和明确问题。
- 不要把历史旧 RVA 当当前 build 事实。
- 不要把用户 private-room softlock 报告当已复现事实；当前证据不足，且 broad whitelist 不应进入主线。

## 6. 下一步

优先级:

1. 保持当前 voice-entry -> PostEventID 二段式拦截，继续收样本。
2. 精简源码里的 heavy `[voice-entry-*]` dump，只保留能判断身份相关性的日志。
3. 在 StartTalk/ref-pack 层加窄 probe，目标是把 `qword,qword / pack[2] / pack[3]` 和 subtitle/voice 两侧同窗相关起来。
4. 用 F8 单动作 session 验证三类样本:
   - Dollman random gameplay chatter
   - throw/recall/equip
   - 非 Dollman / private-room / cutscene
5. 只有当同一身份能同时解释 subtitle pair 和 voice-entry/PostEventID 时，才把它提升成最终规则。

当前判断:

- 继续向上扒是对的，但方向必须是 identity/ref-pack/selector。
- 继续补末端 eventId 只能止血，不能达成终极目标。

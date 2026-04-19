# DollmanMute 字幕屏蔽研究笔记

> 最后更新: 2026-04-19
> 当前版本: **v3.17 (gameplay Dollman 字幕精确过滤 + dispatcher 语义修正)**

## 0. TL;DR

- **音频静音**: Wwise `PostEventID` 黑名单拦截 (legacy 路径, ini 开关可关)
- **字幕 UI 咽喉**: `sub_140780710` = `GameViewGame::ShowSubtitle`, 所有字幕显示唯一下游
- **字幕上游 producer**: `sub_1403873E0` = `StartTalkFunction` vtable slot 15 (chatter 通路)
- **gameplay 主链 (v3.15 静态继续收敛)**: `sub_140F04620` (event hash 入口 / pending 写入) → `sub_140F01F90` (pending/state 调度) → `sub_140F000B0` (9-state emitter / jump table) → `sub_140DAFAD0` (按 selector 构造 `MsgDSStartTalk`) → `sub_140160ED0` → handler C (`sub_140350960`) → SoundHelper → StartTalkFunction → ShowSubtitle
- **v3.17 关键修正**: `sub_1403857E0` 不是“统一 talk dispatcher”，而是 **gameplay-only 的字幕 payload packer / broadcaster**。实机日志已对齐出 `a1[1] == ShowSubtitle.p7 (speaker LocalizedTextResource*)`、`a1[2] == ShowSubtitle.p6 (text LocalizedTextResource*)`；因此这里已经在**字幕侧**，不再是音频/字幕统一拦截点
- **当前精准字幕规则**: `ShowSubtitle` 侧用 `(speaker_tag=0x12B6F, caller_rva=0x38598B)` 屏蔽 gameplay Dollman；同 speaker 在 cutscene 走 `caller_rva=0x202F16F`，放行
- **gameplay direct emit 显式 selector 已收敛成 5 个**: `0x11 / 0x12 / 0x17 / 0x14 / 0x05`
- **throw/recall 实测走 C (MsgDSStartTalk)**(v3.15 实机修正,之前推测 B 错误)
- **分类机制修正**: producer 层 deref `*(*(this+200)+72)` 读 `words[1] >> 32` 当 hi32 tag; 它更像 producer / SoundHelper 摘要,不是 gameplay 架构根判别轴
- **新静态结论**: `sub_140F0DCA0` 是 gameplay talk state 对象的唯一 ctor/init 点(当前只见 1 个 caller = `sub_140E31AD8` @ `0x140E31AF1`), `+0x660 / +0x6B0 / +0x700` 不是外部资源表,而是对象内 inline control window
- **live 进程修正**: 当前找到的 `0x2e5e6283000` 是 `DSCameraMode` 基类实例,但 `0x2e5d3213000` 不是 camera owner,而是 `DSPlayerState`; `DSCameraMode` 自身同时回链到 `DSThirdPersonPlayerCameraComponent` / `CameraEntity` / `DSPlayerEntity`
- **ShowSubtitle payload 修正**: `p6/p7` 不是“神秘 tag 对象”,而是 RTTI=`LocalizedTextResource`; `+0x20` 直接是实际文本指针, `+0x28` 是长度。也就是说 runtime 已经能直接读出字幕正文和 speaker 名
- **当前可直接读出的 live 样本**: `p6 tag 0x1F4 -> "My time to shine." / "How'd I do, Sam?"`, `p7 tag 0x12B6F -> "Dollman"`, `p7 tag 0x122A8 -> "Sam"`, `p6 tag 0x222C -> "Take care not to attract the enemy's attention."`, `p6 tag 0x4045 -> 任务失败文案族`
- **marker 路径修正**: `0x4AB17768 / 0x3126CDE8` 的 live 命中周围是 `ShadingGroup / VertexArrayResource / SkeletonAnimationResource / ModelPartResource`, 更像模型/骨骼/marker 资源簇,不是直接的 talk family key
- **当前 hotkey**: J/K/N/M 只是运行时手工 mute 分组,不等于最终 family 命名; 当前已确认 **J 覆盖 throw/recall/换帽/换眼镜/野外休息**, **N 至少覆盖拿出狙击枪**, **F8 session 分隔, F9 清空 log**, F12 全清 mute
- **非目标 / 架构外未覆盖**: 休息室剧情对话 (`DSTalkManager` 独立通路)、任务失败字幕 (死亡 category 路径)
- **迭代效率**: v3.15 proxy 文件触发 + `build.ps1` 自动 unload/copy/load,不再需要按 F10

## 1. 当前已安装的 Hook

| 入口 | 函数 | 作用 | 装载条件 |
|---|---|---|---|
| `PostEventID` export | Wwise 音频事件入口 | eventId 黑名单 (legacy 音频) | ini `EnableDollmanRadioMute=1` |
| `0x00C73DF0` | DollmanRadio.PlayVoiceByControllerDelay | gun/hat/glasses/chatter 音频 | ini `EnableDollmanRadioMute=1` |
| `0x00DAC1B0` | DollmanVoiceDispatcher | 内层 dollman 语音分发 | ini `EnableDollmanRadioMute=1` |
| `0x00780710` | GameViewGame::ShowSubtitle | 字幕 UI 咽喉, 按 TLS family flag 过滤 + 关联 last_builder (跨线程可能失效) | 永久装 |
| `0x003873E0` | StartTalkFunction.slot15 producer | 分类 speaker → 设 TLS family flag | 永久装 |
| `0x00350050` | Builder A = MsgExpressSignal handler | probe log + caller RA + msg payload dump | ini `EnableBuilderProbe=1` |
| `0x00350460` | Builder B = MsgStartTalk handler | probe log + caller RA + msg payload dump | ini `EnableBuilderProbe=1` |
| `0x00350960` | Builder C = MsgDSStartTalk handler | probe log + caller RA + msg payload dump | ini `EnableBuilderProbe=1` |
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

### 2.6.5 ShowSubtitle payload 的 `p6/p7` = `LocalizedTextResource`

这轮 live 内存把 `[show]` 里的 `p6/p7` 直接坐实了:

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
- 0x140b5beb8 / 0x140b5cd45 (**U1/U2 未定类 caller**,但调 wrapper + 388950 = 结构 gameplay-like)
- 0x140c794d9 (sub_140C79480 新通路,不在 0x388950 列表,低优先级)

**`sub_140388de0` 直接 caller 恰好 3 个** = A/B/C → 表明 NotificationQueueImpl 构造是 gameplay 三 builder 的专有事。

**`sub_140388950` 直接 caller 5 个**:
- 0x3533ae (in sub_140353060, gameplay 中转,0x353060 只被 sub_140350C70 gateway 调用)
- 0x470301 (NPC 通路)
- 0x140b5bf31 (U1 未定类)
- 0x140b5cdb6 (U2 未定类)
- 0xc7293a (DSRadio / dollman random)

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
- U1/U2 (0xb5bc70/cc30) 实机何时触发?能否纳入 gameplay builder 族?
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
| 常量 | 值 |
|---|---|
| `k_rva_dollman_radio_play_voice_by_controller_delay` | `0x00C73DF0` |
| `k_rva_dollman_voice_dispatcher` | `0x00DAC1B0` |
| `k_rva_show_subtitle` | `0x00780710` |
| `k_rva_subtitle_producer` | `0x003873E0` |
| `k_rva_builder_a` (MsgExpressSignal) | `0x00350050` |
| `k_rva_builder_b` (MsgStartTalk) | `0x00350460` |
| `k_rva_builder_c` (MsgDSStartTalk) | `0x00350960` |
| `k_rva_builder_u1` | `0x00B5BC70` |
| `k_rva_builder_u2` | `0x00B5CC30` |
| gameplay direct explicit selector | `0x11 / 0x12 / 0x17 / 0x14 / 0x05` |
| gameplay direct 已观测 hi32 | `0x01F4 / 0x222C / 0x4377 / 0x3745` |
| other 已观测 hi32 | `0x7993 / 0x766B / 0x766F` |

### 8.4 vtable / 静态地址参考
- `0x143188e48` — GameViewGame vtable 起点
- `0x143188e88` — slot +0x40 → `sub_140780710` ShowSubtitle
- `0x143188e90` — slot +0x48 → `sub_140780810` RemoveSubtitle
- `MsgShowSubtitle::vftable` 明文 qword ref @ `0x140780744`
- `0x140160ED0` — **EntityMessaging dispatcher** (dispatch 指令 @ `0x14016107f`, caller_rva 落点 `0x140161082`)
- `0x1441fa7c0` — DSOnTileEntityVariableComponentResource handler table 起点 (10 条 slot × 16B)
- `0x143135930` — `DSTalkInternal::StartTalkFunction` vtable
- `0x143132678` / `0x143132688` — `DSTalkComponentRep::SoundHelper` primary / secondary vtable
- `0x143135548` / `0x1431354B8` — `Simple/DummySoundInstanceWrapper` vtable
- `0x1431358E0` / `0x143135368` — `NotificationQueueImpl` primary / secondary vtable

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
4 个 Instance 变体里 Dollman/Player 已测 (v2.2 / v2.5), Entity 抽象不可构造, **只剩 Speaker 没测**。828 字节 (Dollman PVCD 的 5 倍), 函数体内直接构造 `DSTalkInternal::NotificationQueueImpl` (RESEARCH 早期标注的字幕可疑类)。

双分支:
- `a3 == 0`: closure 入队 `this + 8168` (和 Dollman PVCD 对称)
- `a3 != 0`: 遍历 `*(int*)(this+48)` 批量 dispatch 经 `sub_140388E60`

**若验证**: 先 log-only 3 次 throw/recall 看命中 + a3 分布, 再决定拦截策略。**不要一上来 return 0** (避免再次 "0 hits" 翻车)。

### B.2 DSTalkManager 上游 hook (通路 ③)
RTTI `StartTalk@DSTalkManager@@` @ `0x1460BE4DC`。Decima hash RTTI 让直接 xref 失败, 需走 COL 反推 (参考 Speaker vtable 的反推方法)。若 hook 上, 应同时 kill 休息室对话字幕 + 音频。

### B.3 其余 7 条 handler table slot
table @ `0x1441fa7c0` 里除了 A/B/C (slot 5/6/7) 和前 5 条的 `sub_140354CC0/C60/BD0/B20/AE0` + 尾 2 条 `sub_1403549C0/960`。这些 thunk 都是 2-arg __fastcall,装 log-only hook 的成本同 A/B/C,若 J/N probe 有漏网行为可一次补齐 5 条。

### B.4 sub_140C79480 (0x3896f0 的新 caller)
v3.14 静态发现 `sub_1403896f0` 有 7 个 caller,其中 `0x140c794d9 in sub_140C79480` 不在 0x388950 五 caller 列表里,是**全新通路**。低优先级,但若 U1/U2 实测命中少,此点应当入第二批探针。

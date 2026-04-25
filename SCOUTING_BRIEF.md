# DollmanMute 当前交接简报

> 最后更新: 2026-04-26
> 面向对象: 新接手的 LLM / 逆向 agent / coding agent
> 当前分支: `research-safe-platform`
> 当前工作构建: `research-v3.25c-subtitle-decode-fix`
> 当前状态: **字幕 sender pair 已实锤命中并能精准压掉 Dollman gameplay 台词；但当前实验 build 仍会误伤主角呼喊/动作**

## 0. 先校准地址口径

- 新开的 IDA 数据库已经和 live `DS2.exe` 对齐：当前分析目标是 `DS2.exe v1.5.68.0`。
- 但 `auto_analysis_ready=false`，所以当前最稳的写法不是“死记旧 RVA 名”，而是“真实函数起点 + 历史 probe 偏移”。
- 这份 brief 里仍出现的 `0x00DAFAD0 / 0x003857E0 / 0x00780710` 一类写法，默认都按**历史内部偏移**理解，不自动等于函数入口。
- 当前主线需要抓的是语义层，不是再被旧地址幻觉拖回去。

## 1. 先看这 8 条

- 当前唯一目标是: **把 Dollman 的 gameplay 语音和字幕压掉，并尽量不误伤其他系统**。
- 不要把任务理解成“维护一个能启动的 Mod”或者“反复讨论平台层风险”。平台只是工具，不是目标。
- 当前最重要的 gameplay 主链已经钉死: `gameplay action -> sub_140F04490(+0x190 历史点 0x140F04620) -> sub_140F01F90 -> sub_140F000B0 -> sub_140DAF8A0(+0x230 历史点 0x140DAFAD0)`。
- 当前最关键的 runtime 进展也已经坐实: `ShowSubtitle` sender 侧现在能直接解出 `p6/p7`，并且 `caller_rva=0x385C1B + speaker_tag=0x12B6F` 已经实机命中过 `Muted subtitle surface=sender`。
- 但这不是终点。用户实机反馈显示：在当前实验 build 下，**Dollman gameplay 台词虽然能被很好地压掉，但主角也会变成哑巴，按呼喊键连动作都没有**。这说明当前方案虽然摸到了正确字幕面，但还有别的运行时拦截面在误伤玩家行为。
- 这条主链现在已经摸到命名 owning component: `DSElevenMonthBBControllerComponent`，并且露出了 `EDSElevenMonthBBReactionEvent`、`TargetEvent`、`ForceToOverride`、`loop_time` 这些更上层语义。
- `DSDollmanTalkManager` 当前更像 story-demo / wakeup / private-room / lifecycle 侧系统，不是 gameplay chatter 主轴。
- `ShowSubtitle`、`StartTalkFunction`、`DSTalkManager` 这些下游节点仍然重要，但现在不是第一突破点；真正该继续往上摸的是 component / reaction / selector 语义层。
- 当前 runtime 常驻观测点仍然是 `SelectorDispatch`（历史点 `0x00DAFAD0`，当前真实函数起点是 `sub_140DAF8A0`）；它是我们和 live gameplay 之间最干净的钉子。
- `build.ps1` 负责编译、安装、打包、热更新，但**不会覆盖已存在的 game-root `DollmanMute.ini`**。任何新增键都要考虑这一点。

## 2. 当前工作区与环境

- Repo:
  `C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\mods\DollmanMute`
- Game root:
  `C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH`
- 关键日志:
  `DollmanMute.log`
  `DollmanMute.proxy.log`
- 构建命令:
  `.\build.ps1`
- 当前本地工作区里已经有研究改动:
  `RESEARCH.md`
  `build.ps1`
  `src/core_main.c`
  `SCOUTING_BRIEF.md`

## 3. 当前 runtime 底座

当前工作构建默认常驻的 runtime 点已经变成两层:

- `SelectorDispatch` (`sub_140DAF8A0 + 0x230`, 历史点 `0x00DAFAD0`)
- `GameViewGame.SubtitleRuntime` (`sub_140780690`)
- `GameViewGame.ShowSubtitleSender` (`sub_140780740`)
- `GameViewGame.RemoveSubtitleSender` (`sub_140780840`)

这意味着当前底座的职责很明确:

- 继续观察 selector / sentence-group emit
- 继续观察当前 build 真正命中的字幕 sender/remove 链路
- 继续对齐 gameplay 语义和 live 行为
- 给更上层静态探索提供最少但足够的 runtime 锚点

不要把当前底座理解成“最终方案”，它只是主线推进用的平台。

## 4. 当前默认配置

这些默认值已经固化在 `src/core_main.c`:

```ini
[General]
Enabled=1
VerboseLog=0
EnableDollmanRadioMute=1
EnableThrowRecallSubtitleMute=0
ActiveSubtitleStrategy=1
EnableSubtitleRuntimeHooks=1
EnableSubtitleFamilyTracking=0
EnableSubtitleProducerProbe=0
EnableBuilderProbe=0
EnableSelectorProbe=1
EnableTalkDispatcherProbe=0
ScannerMode=0
```

注意:

- 代码默认值不等于磁盘 ini 一定有这些键。
- 实际生效配置要同时看代码默认值和 game-root 现存 ini。
- 如果别的 agent 新增配置键，必须明确告诉接手者“测试依赖代码默认值”还是“依赖磁盘 ini 已补齐”。

## 5. 运行时策略热键速查

- `F1 = observe`
  只记录，不实际静音。用它做对照。
- `F2 = pair`
  当前代码已更新成 `caller_rva=0x385C1B` + `speaker_tag=0x12B6F`。这条规则已经在当前 build 上实锤命中过 `Muted subtitle surface=sender`。
- `F3 = callerOnly`
  当前代码只看 caller `0x385C1B`。
- `F4 = speakerOnly`
  当前代码仍只看历史 speaker tag `0x12B6F`。当前 sender 路径下还没重新坐实。
- `F5 = selectedFamily`
  只看当前 family 选择，不看 caller / speaker 精确对。`J/K` 控 throw/recall，`N/M` 控 dialogue；但当前 sender 路径下 family 识别还没重新坐实。
- `F6 = pairOrSelectedFamily`
  命中当前 pair，或者命中当前 family 选择，就静音。

实操理解:

- `F1` = 纯观察
- `F2` = 当前主力精确规则
- `F3/F4` = 单轴放宽试验
- `F5` = family 轴试验
- `F6` = 精确规则 + family 兜底

## 6. 当前已坐实的语义锚点

### 6.1 gameplay 主链

```text
gameplay action
  -> sub_140F04490 (+0x190 历史点 `0x140F04620`)
  -> sub_140F01F90
  -> sub_140F000B0
  -> sub_140DAF8A0 (+0x230 历史点 `0x140DAFAD0`)
  -> MsgStartTalk / MsgDSStartTalk builder
  -> StartTalkFunction
  -> sub_1403857E0
  -> ShowSubtitle
```

### 6.2 当前真正值得抓住的节点

- `sub_140F04490 (+0x190 历史点 = 0x140F04620)`
  更像 `event hash -> reaction family(+0x534)` 归一化入口。
- `sub_140F000B0`
  `family -> selector/control-state` 主解释器。
- `sub_140DAF8A0 (+0x230 历史点 = 0x140DAFAD0)`
  句组 selector 发射点；当前 direct gameplay 已确认显式 selector 有 `0x11 / 0x12 / 0x17 / 0x14 / 0x05`。
- `DSElevenMonthBBControllerComponent`
  当前主链的 owning component。
- `EDSElevenMonthBBReactionEvent`
  当前主链上层反应事件名。
- `TargetEvent / ForceToOverride / loop_time`
  当前已经摸到的 component/resource 字段语义。

### 6.3 当前不该再当主轴的分支

- `DSDollmanTalkManager`
- `DSTalkManager`
- `SubtitlesProxy`
- `ShowSubtitle` 末端过滤本身

这些分支不是没价值，而是更适合做验证、补证、兜底，不适合继续承担“往上摸顶层语义”的主线任务。

## 7. 当前新发现的 owning component 边界

已经确认的命名边界:

- `0x143242BE8` = `DSElevenMonthBBControllerComponentSymbols::vftable`
- `0x143242CF8` = `DSElevenMonthBBControllerComponent::vftable`
- `0x143248800` = `DSElevenMonthBBControllerComponentResource::vftable`
- `0x14434CEC0` 全局对象直接带字符串:
  `DSElevenMonthBBControllerComponent`

注册与初始化里已经露出的内容:

- `DSElevenMonthBBControllerComponentSymbols`
- `UUIDRef_DSElevenMonthBBControllerComponent`
- `EDSElevenMonthBBReactionEvent`
- `TargetEvent`
- `ForceToOverride`
- `loop_time`
- `RUNNING_REACTION_EVENT_ST / LP / ED`
- 一整组 `EVENT_BB_*` reaction 名和 facial animation 名

这条边界的意义:

- 我们现在摸到的是 component/resource 语义层，不再只是 selector case 或匿名状态机。
- 类名仍然带明显的 BB / legacy 风格，后续不能只凭名字做排除。
- 真正要做的是把这层 reaction / selector / resource 行为继续和 live Dollman gameplay chatter 一一对齐。

## 8. 当前代码里保留的东西

### 8.1 当前常驻

- `SelectorDispatch` hook
- `SubtitleRuntime` / `ShowSubtitleSender` / `RemoveSubtitleSender` hooks
- proxy/core 热更新框架
- `build.ps1` 的自动安装 / 打包逻辑
- `F8` 5 秒窗口机制
- `F9` 清 log
- `EnableTalkDispatcherProbe` 独立开关

### 8.2 当前待命实验项

- `TalkDispatcher`
- `StartTalkFunction.slot15 producer`
- `StartTalk init`
- `GameplaySink 0x350C70`
- `Builder A/B/C/U1/U2`

这些点的意义是“按需实验项”，不是当前 brief 的主舞台。

## 9. 给其他 agent 的工作规则

- 第一优先级不是平台层反复缠斗，而是继续把 Dollman gameplay chatter 的语义顶层摸清楚。
- 主线继续往 `sub_140F04620` 上面走，不要再把精力下沉回 `DSTalkManager` / `ShowSubtitle` 末端。
- 每次实验都要明确自己是在验证哪一层:
  `reaction event`
  `family`
  `selector`
  `builder`
  `producer`
  `subtitle tail`
- 新 probe 保持单独开关，日志里要能一眼看出到底是谁在说话。
- 如果修改 ini 结构，记住 `build.ps1` 不会覆盖 game-root 已存在的 ini。
- `RESEARCH.md` 记深历史和链路细节；这份 brief 只记当前应当怎么打。

## 10. 当前最合理的后续方向

### P0. 把 `DSElevenMonthBBControllerComponent` 和 live Dollman 行为对齐

优先把下面几件事补实:

1. `EDSElevenMonthBBReactionEvent` 的枚举值 / reaction slot / live 行为对齐
2. `TargetEvent / ForceToOverride / loop_time` 怎么影响 selector 进入
3. `sub_140F04620` 里的 event hash 到底如何落入 reaction family
4. 哪些 reaction 最终落到当前已见的 selector `0x11 / 0x12 / 0x17 / 0x14 / 0x05`

### P1. 找到比 selector 更高、但仍然足够精确的静音切点

理想目标不是在 UI 末端补刀，而是在下面这些层里找到最合适的切口:

- reaction event
- family
- selector family owner
- builder 输入语义

要求是:

- 只打 Dollman gameplay chatter
- 不误伤 cutscene
- 不误伤 Sam / NPC 普通对话
- 不依赖末端 UI payload 才能识别

### P2. 用下游节点做验证，不要让下游节点主导方向

下游节点仍然可以用来验证:

- `Builder B/C`
- `StartTalkFunction`
- `sub_1403857E0`
- `ShowSubtitle`

但它们现在的角色是:

- 验证上游判断是否正确
- 补 live 样本
- 给最终 mute 规则做闭环

不是让探索方向重新回到底层尾部。

## 11. 和 `RESEARCH.md` 的分工

- `RESEARCH.md`
  深历史、静态链路、旧实验、老分支、架构理解、当前新增 owning component 结论
- `SCOUTING_BRIEF.md`
  当前接手者第一步该盯哪条线、哪些是主线、哪些只是辅助线

如果新 agent 时间很少:

1. 先读这份 `SCOUTING_BRIEF.md`
2. 再读 `RESEARCH.md` 顶部 TL;DR 和新增的 owning component 段
3. 然后直接沿 `sub_140F04620` 往上做静态探索

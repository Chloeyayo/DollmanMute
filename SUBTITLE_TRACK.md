# DollmanMute Subtitle Track

> 最后更新: 2026-04-26
> 范围: Dollman gameplay 字幕上游，不含 cutscene / 通用 UI 噪音
> 目标: 找到比 `ShowSubtitle sender` 更早、更稳、更有语义的 Dollman 字幕阀门

## 1. 当前状态

- 当前字幕静音已经可用，但主要还是靠末端 `sender/remove` pair 在做事。
- 现在的问题不是“能不能静音”，而是“能不能把静音点从 UI sender 上提到更早的语义层”。
- 目前最强的上游候选不是 `ShowSubtitle` 自己，而是 `StartTalkFunction` 状态机，尤其是 `sub_140387CF0` 的 `mode=0`。

## 2. 当前已证实进展

### 2.1 当前稳定命中面

- `GameViewGame` subtitle sender: `sub_140780740`
- paired remove/hide sender: `sub_140780840`
- sender caller RVA: `0x385C1B`
- remove caller RVA: `0x385B32`

当前稳定 live 命中条件:

- `caller_rva=0x385C1B`
- `speaker_tag=0x12B6F`
- `line_tag=0x1F4`
- `family=throwRecall`

### 2.2 当前最有价值的链路重建

目前最清楚的 Dollman throw/recall 样本链是:

```text
stf-post
  -> stf-dispatch mode=5, pack232=[0,0,0,0]
  -> SubtitleHit / Muted subtitle
  -> stf-dispatch mode=0, pack232=[speaker, speaker, line, gameObject-like]
  -> 后续 PostEvent / Wwise event
```

注意:

- `mode=5` 已经能看到 `speaker_tag` / `line_tag`，但 `pack232` 还是空的。
- `mode=0` 不是日志里最早出现的一步，但它是当前观测到的**第一处完整语义包成形点**。
- `pack232` 当前最像:
  - `pack232[0]` = speaker ref
  - `pack232[1]` = speaker-related ref
  - `pack232[2]` = line/body ref
  - `pack232[3]` = gameObject / controller-related ref

### 2.3 当前 sender payload 侧的硬事实

- sender payload 的 `p6` 能稳定解成 line/body。
- sender payload 的 `p7` 能稳定解成 speaker。
- `LocalizedTextResource` 的当前 vtable RVA 是 `0x3448D38`。

这说明 subtitle 面已经不是“只知道 caller”，而是能解出真正的句子和说话人资源。

## 3. 当前已经踩实的坑

### 3.1 不要把 `caller_rva=0x385C1B` 当成 Dollman 专属

- 这条 sender 路是整个 gameplay subtitle family 的公共发送面。
- Sam、Fragile 等非 Dollman 样本也会走这里。

### 3.2 不要把 `line_tag=0x1F4` 单独当过滤条件

- `0x1F4` 在 throw/recall 样本里很稳定，但它不是单独足够的身份。
- 没有 `speaker_tag=0x12B6F` 的配合，误伤风险太高。

### 3.3 不要把 `key112_w1=0xffff00c000000001` 当成 Dollman 身份

- 它在 Dollman throw/recall 样本里出现过。
- 但后续 Sam 样本也拿到了同一个值。
- 所以 `key112_w1` 最多是“样本族线索”，不是稳定身份。

### 3.4 不要把 `mode=5` 当成最终语义入口

- `mode=5` 常常太早，`pack232` 还是全零。
- 真正有完整对象包的点目前是 `mode=0`。

### 3.5 不要再把 sender/remove 当唯一主战场

- sender/remove 很适合做当前 build 的精准静音。
- 但它们太靠末端，不能代表真正的上游源头。

## 4. 当前最合理的下一步

### 4.1 主线

继续围绕 `StartTalkFunction` 状态机往上提:

- `sub_140387670`
- `sub_140387CF0`
- `sub_1403881B0`

### 4.2 具体要钉死的点

1. `sub_140387CF0 mode=0` 的真实语义到底是什么。
2. `pack232[3]` 到底是不是后续 `PostEvent gameObject` 或更高层 controller 对象。
3. 谁在 `mode=0` 之后消费这组 `pack232`。
4. 是否能在不依赖 sender 的情况下，仅凭 `StartTalk` 语义包识别 Dollman subtitle。

### 4.3 工程目标

- 短期目标:
  - 让字幕识别不再依赖 `PostEvent` 兜底
  - 让 `sender` 降格成验证面，而不是主决策面
- 中期目标:
  - 把字幕静音点上提到 `StartTalk / stf-dispatch mode=0` 附近

## 5. 当前验证标准

后续如果说“字幕上游又前进了一步”，至少要满足下面其中两条:

- 同一条 Dollman 字幕能在 `stf-post` 看到正确 `speaker_tag=0x12B6F`
- 同一个 `this` 在 `mode=0` 时拿到非零 `pack232`
- 能证明 `pack232[3]` 和后续对象链是同一个稳定身份
- 非 Dollman 样本不会落入同一组语义判断
- 末端 `sender` 即使关掉，也仍然能在更上游识别并压掉 Dollman subtitle

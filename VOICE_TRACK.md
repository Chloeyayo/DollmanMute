# DollmanMute Voice Track

> 最后更新: 2026-04-26
> 范围: Dollman gameplay 语音上游，不含 cutscene / 广播噪音
> 目标: 找到比当前 `PostEvent + voice-delay-schedule` 更早、更稳、更有语义的 Dollman 语音阀门

## 1. 当前状态

- 当前 gameplay 语音静音是有效的，但本质上还是两层拼接:
  - 一层是 `PostEvent / Wwise eventId` 窄拦截
  - 一层是 `voice-delay-schedule` 早拦截
- 这条线的当前问题不是“有没有效果”，而是“我们还没摸到真正的语音发起源头”。
- 当前最合理的研究方向不是继续沉迷某个固定 `eventId`，而是把语音对象链往上拆到 `request / source / controller`。

## 2. 当前已证实进展

### 2.1 当前最可信的 Dollman 语音主链

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

这条静态结论目前仍然成立:

- `sub_140C73E30` 是 Dollman delay voice 的最早已证实 live seam。
- `sub_140C73EE0` 是更下游的保底后截点，不该再当唯一主切点。
- `sub_140DAC7B0` 是 Player / Dollman 共用 helper，blast radius 很高。

### 2.2 当前 live 层面已经确认的事

- `voice-delay-schedule` 这层确实能拦住语音。
- `PostEvent` 这层也确实抓到了 equip / recall 等 gameplay 语音样本。
- `eventId=448888368 && externalSources=1 && ext0=0x41ac17e30` 是真实的 Dollman random/fall 样本，但目前仍然只是单样本签名，不够提升成整类规则。

### 2.3 当前对 voice 身份的工作模型

当前更可信的口径是:

- `captured_id` 更像路由身份
- `source+0x20` 更像语义身份

也就是说:

- 语音这边未必直接带着 `speaker_tag / line_tag`
- 它更可能是通过另一套 request/source/key 结构去投影同一个 gameplay 身份

## 3. 当前已经踩实的坑

### 3.1 不要把 `voice-delay-schedule` 命中当成“已证明和字幕共源”

- 这轮完整日志里，两次 `voice-delay-schedule` 被拦时，附带的最近 `STF` 上下文其实都是 Sam。
- 这说明当前日志只能证明“在这里被拦住了”，不能证明“这里和字幕使用了同一个上游对象”。

### 3.2 不要再把 closure capture 当主突破口

- closure 这层的信息量已经被证明太少。
- 在很多样本里它已经太晚，或者关键字段已经丢失、归零。

### 3.3 不要 blanket mute `sub_140DAC7B0`

- 这是共用 helper。
- 对 Player / 其他角色的误伤风险过大。

### 3.4 不要把固定 `eventId` 当架构终点

- `eventId` 很适合当前 build 的窄拦截和止血。
- 但它只是末端事件，不是共同源头本身。

### 3.5 不要继续把主线压回 `C73EE0 / DAC7B0 / DAC910 / DAA410`

- 这些点对理解结构仍然有价值。
- 但对“当前 build 下语音真正的更高层源头”来说，已经不是第一主线。
- `C73E30` 以上的 request/source/controller 才是更值得继续上的面。

### 3.6 不要把最近 `STF` 时间窗相关性当强证据

- 时间窗相关只能做辅助线索。
- 它不能替代对象级、字段级或容器级的一一对应。

## 4. 当前最合理的下一步

### 4.1 主线

从 `sub_140C73E30` 往上拆语音对象链:

- `instance`
- `controller_index`
- `request_ref`
- `source`
- `source+0x20`
- payload / sentence key / dedupe key

### 4.2 要钉死的对象关系

1. `voice-delay-schedule instance` 到底代表什么对象。
2. 这个对象是否能追到稳定的 controller / gameObject / request 容器。
3. `source+0x20` 是否能和字幕侧的 line/speaker 资源链做稳定相关。
4. `captured_id` 是否只是路由键，还是还能回溯到更高层的 gameplay identity。

### 4.3 当前最值得证明的一件事

不是“再多抓几个 eventId”，而是:

- 证明 `C73E30` 上游的 voice request/source/controller
- 是否和字幕侧 `StartTalk / pack232 / ref-pack` 中的某个对象是同一实体

如果这一步打通，语音线和字幕线即使不共享一个函数入口，也已经共享了同一个 gameplay 身份容器。

## 5. 当前工程口径

- `PostEvent` 样本族:
  - 当前保留为可靠 fallback
  - 不再宣称它就是最终根源
- `voice-delay-schedule`:
  - 当前保留为有效静音点
  - 不再拿它附带的最近 `STF` 上下文当共源证明
- `C73EE0`:
  - 当前保留为保底点
  - 不再当第一主线

## 6. 当前验证标准

后续如果说“语音上游研究前进了一步”，至少要满足下面其中两条:

- 能从 `C73E30` 往上读到稳定 request/source/controller 对象
- 同类 Dollman 样本在这组对象里复现同一身份容器
- 非 Dollman 样本不会落进这组对象判定
- 不依赖固定 `eventId`，仍然能在更上游识别 Dollman 语音
- 能把“为什么当前这个语音是 Dollman”解释成对象/字段关系，而不是时间窗碰巧命中

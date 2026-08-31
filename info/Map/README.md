# info/Map 说明

本目录存放 Archify 流程图/架构图。

- [`media_pipeline_archify.md`](media_pipeline_archify.md)：视频、音频、图像三条媒体链路流程图（Mermaid）。
- [`sd-playback-chain.html`](sd-playback-chain.html)：SD 卡 → 屏幕 / 喇叭 播放链路交互式数据流图（Archify 渲染，支持明暗主题、缩放、聚焦、导出）。源规格：[`sd-playback-chain.dataflow.json`](sd-playback-chain.dataflow.json)。
- [`sd-resource-rule.html`](sd-resource-rule.html)：**SD 卡资源判断规则**交互式架构图（制作资源参考：后缀→格式→限额→输出，含三类资源规范卡片）。源规格：[`sd-resource-rule.architecture.json`](sd-resource-rule.architecture.json)。
- [`production-unlock.html`](production-unlock.html)：**生产加密授权解锁流程**交互式生命周期图（Secure Boot/Flash Encryption 门 → eFuse 永久解锁位 → 一次性令牌验签）。源规格：[`production-unlock.lifecycle.json`](production-unlock.lifecycle.json)。

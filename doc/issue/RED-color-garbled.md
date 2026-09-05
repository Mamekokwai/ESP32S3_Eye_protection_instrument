# RED 颜色异常 — 已解决

## 现象

`spilcd_fill_raw(0, 0, 320, 320, 0xF800)` 在 10MHz PCLK 下显示蓝色/噪点，非纯红。

## 根因

**ESP32 I80 控制器 PCLK 分频比问题。**

时钟源 PLL160M (160MHz)，PCLK = 160 / (div_num + 1)：

| PCLK | div | 结果 |
|------|-----|------|
| 5 MHz | 31 | 纯蓝 |
| 10 MHz | 15 | 蓝底红噪点 |
| **20 MHz** | **7** | **正常** |

10MHz (div=15) 时 WR 时序采样窗口偏移。20MHz (div=7) 时序正确。

**排除地弹的原因**：
- 0xFFF0 / 0xF800 在同一频率下均正常（同一频率下同一个 0xF8 字节）
- 0xFFFF (全 1) 正常（地弹理论下应最严重）
- 降频 (5MHz) 反而更差（不符合信号完整性规律）

## 修复

```c
// components/BSP/SPILCD/spilcd.c
.pclk_hz = 20 * 1000 * 1000,   // 20MHz, 分频比 7
.flags = {
    .swap_color_bytes = 1,      // 8-bit 总线 RGB565 字节交换
},
```

无需其他修复。不要降频。

## PCLK 频率参考

时钟源 PLL160M (160MHz)，常用分频：

| PCLK | div |
|------|-----|
| 20 MHz | 7 |
| 26.7 MHz | 5 |
| 32 MHz | 4 |
| 40 MHz | 3 |

JD9855 8080 模式预计支持到 20-30MHz，可试 26.7MHz 或更高。

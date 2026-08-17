# ESP32-S31 VoCat「喵伴」开发板 引脚速查

> 板型：`ESP-VoCat-S31-CoreBoard-V1_0`（嘉立创 OHHWHub 工程，主控 `ESP32-S31-WROOM-1`）
> 本文档由 **EasyEDA Pro 原理图网表（Protel2 导出）逐项核对** 生成，核对时间 2026-08-12。
> 用途：后续软件适配（摄像头 / 音频 / 显示 / 触控）的权威引脚来源，避免再出现「引脚冲突 / SCCB 探测失败」。

**置信度图例**
- ✅ 网表已核对：原理图网表中的网络名 ↔ MCU GPIO ↔ 器件引脚三元组一致，高置信。
- ⚠️ 代码待验证：网表正确，但本工程代码层尚未真正点亮/跑通（主要是摄像头 DVP）。
- 🔧 实测点亮：已在固件中验证可用（LCD/I2S/触摸/音频）。

---

## 1. 系统 / 启动

| 网络名 | GPIO | 方向 / 有效电平 | 器件 | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `ESP_BOOT` | GPIO61 | 输入，下拉 | ESP32-S31 BOOT | ✅ 网表 | **不是 S3 的 GPIO0**！拉高进下载模式 |
| `USB_DM` | GPIO33(USB1_DN) | — | USB_OTG | ✅ | 专用引脚，勿复用 |
| `USB_DP` | GPIO23(USB1_DP) | — | USB_OTG | ✅ | 专用引脚 |
| `VBAT_IN` | — | 电源 | CN100/H100 | ✅ | 电池 / 5V 输入 |
| `VIN` | — | 电源 | H101/U2/U106 | ✅ | 主 5V |

> 无独立状态 LED（GPIO17 是 `CODEC_PWR_CTRL`，不是 LED）。

---

## 2. I2C 总线（只有一条 BAT 总线）

所有 I2C 设备（ES8389 / CST816S 触摸 / SC101IOT 摄像头 SCCB / BMI270 IMU）**共用 GPIO0/1**，经 2.2kΩ 上拉到 `MCU_3V3`（R10/R11）。ES8389 另经 Q103（LBSS138DW1 电平转换）挂到 `CODEC_3V3` 域，地址 0x20。

| 网络名 | GPIO | 设备 | 地址 | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `BAT_SCL` | GPIO0 | 公共 SCL | — | ✅ 网表 | 摄像头 SCCB SCL(P1-5)、BMI270 SCX、CST816S SCL(U101-6)、ES8389 SCL(Q103) |
| `BAT_SDA` | GPIO1 | 公共 SDA | — | ✅ 网表 | 摄像头 SCCB SDA(P1-3)、BMI270 SDX、CST816S SDA(U101-5)、ES8389 SDA(Q103) |
| `AUDIO_I2C_SCL` | (Q103-4) | ES8389 SCL 侧 | 0x20 | ✅ | 电平转换后到 ES8389 |
| `AUDIO_I2C_SDA` | (Q103-1) | ES8389 SDA 侧 | 0x20 | ✅ | 电平转换后到 ES8389 |

> ⚠️ **多设备同总线**：摄像头 SCCB 与 ES8389 I2C 同一组 GPIO0/1。摄像头上电（`CAM_EN` 拉低）后才能 SCCB 探测；探测失败历史上是「CAM_3V3 未上电就探测」导致，非引脚错。

---

## 3. I2S 音频（ES8389，DAC+ADC 共用 I2S port 0，MCLK-less 模式）

| 网络名 | GPIO | 方向 | 器件引脚 | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `ES_SCLK` | GPIO16 | 输出(BCLK) | ES8389-SCLK | 🔧 实测 | I2S 位时钟 |
| `ES_DLRCK` | GPIO18 | 输出(WS) | ES8389-LRCK | 🔧 实测 | 帧同步（LRCLK） |
| `ES_ASDOUT` | GPIO19 | 输入(ADC) | ES8389-ASDOUT/AD1 | 🔧 实测 | 麦克风数字输出 → MCU（record） |
| `ES_DSDIN` | GPIO35 | 输出(DAC) | ES8389-DSDIN | 🔧 实测 | MCU → 喇叭数字输入（playback） |
| `MCLK` | — | — | — | ✅ 网表 | **MCLK-less**：R6(0Ω) 仅串联在 DSDIN 通路，无独立 MCLK 给 codec |

> I2S 格式：实测 **32-bit / 16kHz / 立体声**（与官方 v014 固件一致）。采样率设 16000 时 ES8389 coeff 表可命中（48000 亦可）；不要误设为 16-bit。

---

## 4. 音频控制 / 电源

| 网络名 | GPIO | 有效电平 | 器件 | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `CODEC_PWR_CTRL` | GPIO17 | 输出，低有效使能 | U2 HE9073A33M5R LDO → `CODEC_3V3` | ✅ 网表 / 🔧 | ES8389 常供电域；**不应配 audio_power**（LDO 自供电，GPIO 仅控制使能） |
| PA `CTRL` | GPIO2 | 高有效 | U106 NS4150B CTRL | ✅ 网表 / 🔧 | 喇叭功放使能。**注意**：ES8389 驱动内部 `pa_pin` 与 `gpio_pa_control` 外设都配 GPIO2，会打印 `gpio: conflict found for GPIO[2]` 警告，**不致命**，保持现状 |
| `LCD_BLK` | GPIO46 | 高有效 | Q101 AO3400A → 背光 LEDK | ✅ 网表 / 🔧 | 屏背光，PWM 或高电平点亮 |
| `LCD_3V3` (电源使能) | GPIO9 | 低有效 | Q100 AO3401A → `LCD_3V3` | ✅ 网表 | `POWER_CTRL`：拉低 → LCD 供电 |

> ⚠️ **坑**：`GPIO44` 是 LCD QSPI **D1**，不是背光；背光是 `GPIO46`。历史多次把两者搞混导致黑屏。

---

## 5. LCD QSPI（ST77916，360×360 圆屏）

| 网络名 | GPIO | 功能 | 器件引脚(U101) | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `LCD_SDA0` (D0) | GPIO38 | QSPI D0 | U101-12 | 🔧 实测 | 数据线 0 |
| `LCD_SDA1` (D1) | GPIO44 | QSPI D1 | U101-13 | 🔧 实测 | 数据线 1（**非背光**） |
| `LCD_SDA2` (D2) | GPIO37 | QSPI D2 | U101-14 | 🔧 实测 | 数据线 2 |
| `LCD_SDA3` (D3) | GPIO45 | QSPI D3 | U101-15 | 🔧 实测 | 数据线 3 |
| `LCD_SCLK` | GPIO39 | QSPI SCLK | U101-10 | 🔧 实测 | 时钟 |
| `LCD_DC` | GPIO40 | 数据/命令 | U101-8 | 🔧 实测 | |
| `LCD_CS` | GPIO43 | 片选 | U101-11 | 🔧 实测 | |
| `LCD_RST` | GPIO60 | 复位(低有效) | U101-21, U101-4 | 🔧 实测 | `reset_active_high=false` |
| `LCD_TE` | GPIO12 | 撕裂同步 | U101-22 | ✅ 网表 | Tear-Effect，避免撕裂 |

> LCD 供电：`POWER_CTRL`(GPIO9) → Q100 → `LCD_3V3`；背光：`GPIO46` → Q101 → LEDK。

---

## 6. 触摸（CST816S，I2C 地址 0x2A）

| 网络名 | GPIO | 功能 | 验证 | 备注 |
| --- | --- | --- | --- | --- |
| `BAT_SDA` | GPIO1 | 触摸 SDA（U101-5） | ✅ 网表 / 🔧 | 与 BAT 总线共用 |
| `BAT_SCL` | GPIO0 | 触摸 SCL（U101-6） | ✅ 网表 / 🔧 | 与 BAT 总线共用 |
| `TP_INT` | GPIO42 | 中断（U101-3） | 🔧 实测 | INT 引脚，下降沿触发 |

> 触摸控制器在 LCD 模组 U101 内部，I2C 经 U101 的 5/6 脚接到 BAT 总线。应用层取句柄：`esp_board_manager_get_device_handle("lcd_touch", ...)` → `dev_lcd_touch_handles_t.touch_handle`。

---

## 7. 摄像头 DVP（SC101IOT，24-pin FPC `P1`，型号 AFC11-S24ICC-00）

> ⚠️ **代码待验证**：网表已逐脚核对，但本工程摄像头尚未点亮（SCCB 历史探测失败）。上电顺序务必：**先 `CAM_EN` 拉低给 CAM_3V3 上电 → XCLK 起振 → 再 SCCB 探测**。

### 7.1 数据 / 同步信号

| 网络名 | GPIO | FPC 脚(P1) | 功能 | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `DVP_Y2` | GPIO57 | P1-19 | 数据 D0 | ✅ 网表 | 8-bit 数据线（Y2~Y9） |
| `DVP_Y3` | GPIO49 | P1-21 | 数据 D1 | ✅ 网表 | |
| `DVP_Y4` | GPIO50 | P1-22 | 数据 D2 | ✅ 网表 | |
| `DVP_Y5` | GPIO48 | P1-20 | 数据 D3 | ✅ 网表 | |
| `DVP_Y6` | GPIO56 | P1-18 | 数据 D4 | ✅ 网表 | |
| `DVP_Y7` | GPIO55 | P1-16 | 数据 D5 | ✅ 网表 | |
| `DVP_Y8` | GPIO54 | P1-14 | 数据 D6 | ✅ 网表 | |
| `DVP_Y9` | GPIO47 | P1-12 | 数据 D7(MSB) | ✅ 网表 | |
| `DVP_PCLK` | GPIO51 | P1-17 | 像素时钟 | ✅ 网表 | |
| `DVP_HREF` | GPIO53 | P1-9 | 行有效 | ✅ 网表 | |
| `DVP_VSVNC` | GPIO8 | P1-7 | 帧同步(VSYNC) | ✅ 网表 | **VSYNC=GPIO8，不是 GPIO15** |
| `GPIO52`(XCLK) | GPIO52 | P1-13 | 主时钟输入 | ✅ 网表 | 24MHz，`CONFIG_CAMERA_XCLK_USE_LEDC=y` |

### 7.2 控制 / 电源

| 网络名 | GPIO/PIN | 电平 | 验证 | 备注 |
| --- | --- | --- | --- | --- |
| `CAM_EN` | GPIO10 | **低有效** | ✅ 网表 | Q1 AO3401A PMOS → `CAM_3V3`；拉低给摄像头供电 |
| `CAM_RESET` | P1-6 | 上拉 `AVDD_2V8`(R8) | ✅ 网表 | 默认高=不复位 → **`reset_io = -1`** |
| `CAM_PWDN` | P1-8 | 下拉 GND(R9) | ✅ 网表 | 默认低=上电 → **`pwdn_io = -1`** |
| `CAM_3V3` | — | 电源 | ✅ 网表 | 由 `CAM_EN` 控制 |
| `AVDD_2V8` | P1-4, P1-11 | 2.8V | ✅ 网表 | 模拟电源（CAM_RESET 上拉源） |
| `DVDD_1V5` | P1-10 | 1.5V | ✅ 网表 | 数字核心电源 |

### 7.3 SCCB（I2C）

| 网络名 | GPIO | FPC 脚 | 地址 | 验证 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `BAT_SDA` | GPIO1 | P1-3 | 0x68 | ✅ 网表 | SC101IOT SCCB 地址 `SC101IOT_SCCB_ADDR=0x68` |
| `BAT_SCL` | GPIO0 | P1-5 | — | ✅ 网表 | 与 BAT 总线共用，100kHz |

---

## 8. 其它外设（扩展排针 / IMU / UART）

| 网络名 | GPIO | 功能 | 验证 | 备注 |
| --- | --- | --- | --- | --- |
| `GPIO3` | GPIO3 | BMI270 INT1 | ✅ 网表 | IMU 中断（U102-4） |
| `GPIO6`/`GPIO7` | GPIO6/7 | 触摸 pad（R103/R104） | ✅ 网表 | 预留触摸按键 |
| UART1 | GPIO5(RXD)/GPIO4(TXD) | 串口1 | ✅ 网表 | H101 排针 |
| 扩展 IO | GPIO20/21/22/23/24/25/33/34/36/37* | 排针 H100/H101 | ✅ 网表 | *GPIO37 已用于 LCD，注意复用 |

---

## 9. 踩坑速记

1. **烧录必须 921600 波特率**（COM12）。115200 会在 ~6MB 处 USB 掉线（`PermissionError 13`）。
2. **ES8389 驱动在 `device/es8389/es8389.c`**，不是 `es8388.c`。补丁（重试+400ms 上电延时）会被组件管理器覆盖，需从 `patches/` 重新同步。
3. **多媒体板（esp32_s31_korvo1）配置不可套用**：喵伴板是 QSPI 圆屏 + ES8389 + SC101IOT，引脚完全不同。
4. **`GPIO44`=LCD QSPI D1，不是背光**；背光是 `GPIO46`。黑屏先查这两者。
5. **VSYNC=GPIO8**，历史误记为 GPIO15（GPIO15 实际悬空未连摄像头）。
6. **I2S 用 32-bit**（非 16-bit），与官方 v014 一致；MICBIAS 由 ES8389 open 序列置位（0x62=0x80）。
7. **PA(GPIO2) 双重配置警告**（`gpio: conflict found for GPIO[2]`）已知不致命，勿扩大改动面。
8. **摄像头先上电再探 SCCB**：`CAM_EN` 拉低 → 等 CAM_3V3 稳 → XCLK 起 → 再探测 0x68。否则必报 `Failed to detect camera sensor`。
9. 出图颜色异常**先 dump 原始字节**判断字节序，不要盲加 `__builtin_bswap16`（曾导致红绿蓝错位漩涡）。SC101IOT 输出 YUV422，**无 JPEG 直出**，需硬件 JPEG 编码（`/dev/video10`）。

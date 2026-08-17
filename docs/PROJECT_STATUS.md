# 复刻小智多功能喵伴 · 项目状态与路线图

> 项目代号：`vocat-xiaozhi`（ESP-VoCat-S31 视觉 + 蓝牙音频 AI 萌宠）
> 工程路径：`H:\espwork\esp-projects\vocat-xiaozhi`
> 构建输出：`H:\espwork\vocat-xiaozhi-build`
> 立创EDA 工程：`ESP-VoCat-S31 喵伴：视觉与蓝牙音频的 AI 萌宠_copy`
> 维护者：han ｜ 最后更新：2026-07-30

---

## 1. 环境与构建状态（已验证 ✅）

| 项 | 值 |
|---|---|
| 工具链 | ESP-IDF **6.2.0** (`C:/esp-idf-master`) |
| 目标芯片 | **ESP32-S31** (`IDF_TARGET=esp32s31`) |
| 板级管理 | Espressif `esp_board_manager`（板型 `esp_vocat_s31_board`） |
| 最新构建 | **成功**（2026-07-24 `Project build complete`） |
| 产物 | `xiaozhi_chat.bin` = `0x4B5970` (4.9MB)，分区余量 22% |
| 依赖组件 | esp_xiaozhi、esp_cam_sensor、esp_video、esp_video_codec、esp_codec_dev、esp_board_manager、lvgl 等 |

> 结论：**固件主体已可编译并生成可烧录镜像**，传输/音频/显示框架已打通。

---

## 2. 硬件能力核验（来自真实 PCB 网表，101 个网络）

数据来源：`hardware/esp_vocat_s31_board_full.json`（由立创EDA skill 经桥接实时提取，
PCB 名 `PCB-ESP-VoCat-S31-CoreBoard-V1_0`：383 焊盘 / 984 走线 / 179 过孔 / 101 网络）。

| 功能 | 板载信号（网络名） | 固件是否已配置 |
|---|---|---|
| **音频编解码 ES8389** | `ES_MCLK/ES_SCLK/ES_DLRCK/ES_DSDIN/ES_ASDOUT`、`ES_LOUTx`、`ES_MIC1x/ES_MIC2x`、`AUDIO_I2C_SCL/SDA`、`CODEC_PWR_CTRL` | ✅ 已配（I2S+ I2C） |
| **喇叭/PA 使能** | `GPIO7`（PA）、`GPIO17`（CODEC 供电）、`GPIO9`（LCD 供电）、`GPIO16`（绿灯） | ✅ 已配 |
| **显示屏 (SPI LCD)** | `GPIO4/6/37/39/45`（SPI 数据/时钟）、`VOP/VON/LEDK`（屏偏压/背光） | ✅ 已配（SPI LCD） |
| **触摸 CST816S** | 走 I2C（与音频同总线或独立，需确认） | ✅ 已配 |
| **🔴 摄像头 / 视觉 (DVP)** | `CAM_EN/CAM_PWDN/CAM_RESET`、`CAM_3V3`、`DVP_PCLK/DVP_HREF/DVP_VSVNC`、`DVP_Y2~Y9`（8-bit 并行） | ❌ **未配置** |
| **蓝牙音频** | ESP32-S31 自带 BT；小智走 Wi-Fi 传输（WebSocket/MQTT），A2DP 未接入 | ❓ 待实现 |
| **USB / 电池** | `USB_DM/USB_DP`、`VBAT_IN/VIN`、`BAT_SCL/BAT_SDA`（电量计 I2C）、`CHIP_PU/ESP_BOOT` | 部分 |
| **表情资源** | `emote-assets1.bin`（猫咪表情二进制，见 `H:\espwork\`） | 🟡 资源在，显示层接入中 |

---

## 3. 核心缺口（"多功能"尚未补齐的部分）

### 缺口 A：摄像头（视觉）硬件在板上，固件没接管 ❌
板级配置 `components/gen_bmgr_codes/gen_board_metadata.yaml` **只定义了**
`audio_codec / display_lcd / lcd_touch / gpio_ctrl`，**没有 camera 设备**。
但 PCB 明确布了 8-bit 并行 DVP 摄像头接口（`DVP_Y2~Y9` + PCLK/HREF/VSYNC +
`CAM_EN/PWDN/RESET`）。依赖里虽已含 `esp_cam_sensor / esp_video / esp_video_codec`，
但没有任何板级 camera 设备把它们接到具体 GPIO。

→ 这是"视觉"功能缺失的根因，也是下一步最该补的。

### 缺口 B：蓝牙音频（A2DP）❓
ESP32-S31 支持经典/低功耗蓝牙。当前小智走 Wi-Fi 传输语音流，尚未做板载
蓝牙音频（如 A2DP Sink/Source 接耳机/音箱）。属"蓝牙音频"卖点的待办。

### 缺口 C：喵咪表情/人格呈现 🟡
`emote-assets1.bin` 已存在（猫脸表情帧），需在显示层把表情动画与对话状态
（聆听/思考/说话）联动，做出"萌宠"人格。

---

## 4. 下一步路线（建议优先级）

1. **补齐摄像头 GPIO 映射**（阻塞项）
   - 需从原理图 **MCU 页** 精确提取 ESP32 引脚 ↔ `DVP_*`/`CAM_*` 网络映射。
   - 注意：当前立创EDA bridge 直接切文档会 500，需用稳妥方式提取
     （见 `H:\espwork\tools\easyeda\query_board.py`，必要时逐页查询）。
2. **在 `gen_board_metadata.yaml` 增加 camera 设备**（DVP 8-bit：XCLK、PCLK、
   HREF、VSYNC、DATA0~7=Y2~Y9、CAM_EN/PWDN/RESET/3V3），并在
   `board_manager.defaults` 打开对应 `CONFIG_ESP_BOARD_DEV_CAMERA_*`。
3. **固件接入视觉**：用 `esp_cam_sensor` + `esp_video` 初始化摄像头，
   接到小智的"视觉"能力（如拍照/图生文），体现"视觉萌宠"。
4. **（可选）蓝牙音频 A2DP** 与 **表情动画联动**。
5. 重新 `idf.py gen-bmgr-config -b esp_vocat_s31_board` → `build flash monitor` 验证。

---

## 5. 目录约定（每个项目独立、工具集中）

```
H:\espwork\
├── tools\                 # 通用工具（跨项目复用）
│   ├── README.md
│   └── easyeda\           # 立创EDA 桥接 + 板子提取
│       ├── start_bridge.bat
│       └── query_board.py
├── esp-projects\
│   ├── vocat-xiaozhi\     # ← 本项目（固件源码）
│   │   ├── main\  components\  managed_components\
│   │   ├── hardware\      # ← 板子数据归档（PCB/原理图提取）
│   │   └── docs\          # ← 本文档
│   └── camera-wifi\       # 其他 ESP 项目（独立）
├── vocat-xiaozhi-build\   # 构建输出（与源码分离）
├── easyeda-api-skill\     # 立创EDA skill 仓库（同源已集成进 WorkBuddy）
└── walle-tools\           # 既有项目级工具（保留）
```

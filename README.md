# ESP32-S31-VoCat 喵伴 · AI 萌宠聊天机器人

> 基于 **ESP32-S31-WROOM-1**（乐鑫 2026 新发布的 RISC-V 双核 320MHz 芯片）的离线唤醒 + 云端对话 AI 萌宠，自带圆形触摸屏、DVP 摄像头、ES8389 音频 codec，并支持 **SD 卡 / 在线音乐播放**。

English below.

## 亮点（为什么是 ESP32-S31-VoCat）

- **ESP32-S31 新平台**：RISC-V 双核 320MHz + 硬件 JPEG 编解码 + PPA 2D 加速 + Wi-Fi 6 + BT5.4，算力比传统 S3 更强、功耗更低。
- **真·萌宠表情**：LVGL 9 + esp_emote_expression 驱动的 360×360 圆形屏表情引擎，20+ 内置表情 + 实时字幕，离线也能卖萌。
- **离线唤醒 + 云端对话**：ESP-SR 离线唤醒词（你好喵伴 / 你好小智），接入 xiaozhi.me 流式 ASR + LLM + TTS，云端大模型对话。
- **双模音乐播放**：本地 SD 卡曲目 + **在线音乐搜索播放**（对接公共音乐搜索 API，语音说「播放 XX」即可搜歌播放）。
- **实时摄像头**：SC101IOT DVP 摄像头 → PPA 硬件缩放预览到圆形屏，支持 Web MJPEG 实时画面（手机浏览器直接看），离线帧差视觉分析（运动/挥手检测、番茄钟守护模式）。
- **完整控制中心**：音量 / 亮度 / WiFi 重置 / 配网，30s 连不上网自动重进配网。
- **蓝牙 / MCP**：支持 MCP 工具调用（拍照、控制设备等）。

## 硬件

| 模块 | 说明 |
| --- | --- |
| 主控 | ESP32-S31-WROOM-1（RISC-V 双核 320MHz，Wi-Fi 6 + BT5.4） |
| 屏幕 | 1.85" 圆形触摸屏 ST77916 360×360（QSPI） |
| 音频 | ES8389 codec + NS4150B PA（双麦阵列 + 喇叭） |
| 摄像头 | SC101IOT DVP 摄像头（1280×720） |
| 存储 | microSD 卡（音乐 / 录像） |
| 交互 | BMI270 IMU + 触摸手势（上滑/下滑切页） |

## 功能一览

- 语音对话：离线唤醒 → 流式 ASR → LLM → TTS，表情随情绪变化。
- 音乐：SD 卡本地播放 + 在线搜索播放（多端点兜底，自动解析流地址）。
- 摄像头：实时预览、Web 直播（`/stream` MJPEG、`/snapshot` 抓拍）、离线视觉分析。
- 页面：表情主页 / 功能桌面 / 对话 / 摄像头 / 音乐 / 游戏 / 番茄钟（含守护模式）。
- 七夕彩蛋：连续点击功能桌面 5 次触发红心动画。
- 配网：30 秒连不上自动重进 SoftAP 配网。

## 构建与烧录

环境：ESP-IDF `master`（`C:\esp-idf-master` 或等价），工具链 `riscv32-esp-elf`。

```bash
# 1. 拉取组件（首次）
idf.py set-target esp32s31
idf.py reconfigure            # 由 idf_component.yml 拉取 managed_components

# 2. 生成板级配置（esp_board_manager）
idf.py gen-bmgr-config -b esp_vocat_s31_board

# 3. 编译
idf.py build

# 4. 烧录（921600 波特率更稳定）
idf.py -p COM12 flash
idf.py -p COM12 monitor
```

或直接用仓库自带脚本（已配置好本机环境）：

```powershell
.\build_only.ps1        # 仅编译
.\esptool_flash.ps1     # 编译 + 烧录（COM12, 921600）
```

> 注意：本机 ESP-IDF 工具链版本检查较严格，构建脚本中已设 `IDF_MAINTAINER=1` 把版本检查降级为警告。若改了 `main/boards` 或板级 YAML，需重跑 `gen-bmgr-config` 并同步 `components/gen_bmgr_codes` 生成产物。

## 在线音乐说明

在线播放通过 `app_music_online_search()`（`main/page_music.c`）实现：对多个公共音乐搜索 API 发起 HTTPS 请求，用 cJSON 递归解析出第一个可播放的音频流地址，交给 `esp_audio_simple_player` 解码播放。内置多端点兜底（uomg / vvhan / codelife / Meting），哪个网络能通就用哪个。如需更换/新增 API，改 `page_music.c` 里的 `k_music_apis` 数组即可。

---

# ESP32-S31-VoCat · AI Pet Companion

An offline-wake + cloud-chat AI pet built on the **ESP32-S31-WROOM-1** (Espressif's 2026 RISC-V dual-core 320MHz chip), with a round touchscreen, DVP camera, ES8389 audio codec, and **SD-card / online music playback**.

### Highlights
- **ESP32-S31 platform**: RISC-V dual-core 320MHz, hardware JPEG + PPA 2D accelerator, Wi-Fi 6 + BT5.4.
- **Pet emotes**: LVGL 9 + esp_emote_expression on a 360×360 round screen (20+ emotes + live subtitles), fully offline.
- **Offline wake + cloud chat**: ESP-SR wake word → xiaozhi.me streaming ASR + LLM + TTS.
- **Music**: local SD tracks **and online search-to-play** (voice "play XX" searches and streams).
- **Camera**: DVP live preview via PPA, Web MJPEG streaming, offline vision analysis.
- **Control center + MCP**: volume/brightness/WiFi, 30s auto-reprovisioning, MCP tool calls.

### Build
See the Chinese section above; use `idf.py set-target esp32s31`, `idf.py reconfigure`, `idf.py gen-bmgr-config -b esp_vocat_s31_board`, then `idf.py build flash`.

## License
Component licenses follow their upstream projects (Espressif esp_xiaozhi, esp_board_manager, LVGL, esp-sr, etc.). Project application code: Apache-2.0.

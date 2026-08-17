# 诊断报告：喵伴“亮眼睛、不动、不对话”根因

抓取时间：2026-07-30 21:4x（COM12 boot log → `H:\espwork\boot_log5.txt`，386 行）
固件版本：Project `xiaozhi` v2.4.0，编译时间 Jul 28 2026 16:29，ESP-IDF v6.1-dev

## 现象
设备上电后屏亮，显示一个“眼睛”图标（即 `esp_xiaozhi_chat_display` 默认
`FONT_AWESOME_AI_CHIP` 图标），但：
- 不切换表情（不“动”）
- 没有任何 AI 对话（不“说话”）
=> 等同“废物”。

## 根因（已确认）
设备上当前运行的是**一套旧的、带 bug 的 ESP‑VoCat 自定义板级初始化固件**
（boot log 中 `ESP-VoCat` / `StateMachine` 等自定义 log 标签），
其板级代码在启动时扫描并把 ES8389 音频编解码器的 I2C 总线**错误地绑定到
GPIO10 / GPIO11**：

- boot log 中 I2C 扫描在 GPIO10/11 上“对每个地址 0x08~0x7F 都 ACK”，
  这是**该引脚根本不是真实 I2C 总线**的典型假象；
- 编解码器驱动随后在 GPIO10/11 上对每个寄存器做 I2C 写，全部失败：
  `E (...) I2C_If: Fail to write to dev 20 reg 0x..` + `W GPIO 10 is not usable, maybe conflict`；
- 每个寄存器写超时约 1 秒，累计 **60 秒以上卡在 ES8389 初始化里**，
  设备永远到不了启动聊天（esp_xiaozhi_chat_app）这一步。
=> 没有 chat 事件 → 表情不切换、无对话、屏幕只剩默认眼睛图标。

## 正确配置（来自 board metadata，已核对）
`components/gen_bmgr_codes/gen_board_metadata.yaml` 与生成的 `gen_board_*.c`
给出的才是**与硬件一致**的真实引脚（设备 config 中 `mclk_enabled=false`
是有意为之，ES8389 不需要 MCLK）：

| 功能 | 引脚 |
|------|------|
| I2C (codec + touch) | SDA=GPIO0, SCL=GPIO1 |
| ES8389 I2C 地址 | 0x20 |
| I2S | BCLK=20, WS=18, DOUT=35, DIN=19 |
| CODEC 电源 | GPIO17 (低有效) |
| PA 控制 | GPIO7 |
| LCD 电源 | GPIO9 |
| LED 绿灯 | GPIO16 |
| 背光 PWM | GPIO44 |
| LCD ST77916 | QSPI 360x360, cs=43, dc=40, reset=46(高有效) |
| 触摸 CST816S | I2C, addr=0x2A, int=42 |
| ADC/DAC | ES8389 (DAC+ADC) |

当前 `main/` 源码（`app_main.c` 仅 `nvs → example_connect → esp_xiaozhi_chat_app`）
是**标准 Espressif esp_xiaozhi 流程**，完全使用上述正确引脚，**不含**那个
GPIO10/11 的 bug 扫描逻辑。换句话说：当前磁盘上的源码是对的，只是设备里
跑的还是旧版错固件。

## 结论与修复路线
1. **P1（已定位）**：重建并烧录当前正确源码 → 设备脱离 60s 卡死，
   WiFi 连接 → xiaozhi.me 握手 → 表情随事件切换、AI 对话跑通（覆盖缺口 D 与 C）。
2. **P4（A 摄像头）**：当前 `gen_board_metadata.yaml` 未定义 camera 设备，
   `CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT` 未开 → 摄像头代码是死代码。
   需补 SC101IOT DVP 摄像头设备 + 开 config + 重新生成板级代码。
3. **P5（B 蓝牙）**：当前无 A2DP，需新增 Classic Bluetooth 双向（Sink+Source）音频。
4. **P6**：`idf.py -p COM12 flash monitor`，抓 boot_after_flash.log 验证全部启动标志。

## 待核验项
- WiFi 凭据在 `sdkconfig.defaults` 为 `HONOR / JIN060519`，需与实际热点一致，
  否则 `example_connect()` 会失败导致仍无对话。
- 摄像头 DVP 引脚（XCLK/PCLK/HREF/VSYNC/D0‑D7/EN/PWDN/RESET）目前按官方默认
  顺序推断，需上电后实测核验。

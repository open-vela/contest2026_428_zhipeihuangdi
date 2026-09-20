# Vela-WALL·E 多模态 AI 机器人

## 一、作品简介

Vela-WALL·E 是一款基于 ESP32-P4 的多模态 AI 机器人，集成语音聆听、语音识别、AI 对话、语音合成、摄像头视觉感知、LVGL 图形界面和 STM32 舵机控制。

机器人通过屏幕上的麦克风按钮开始聆听，再次点击后结束录音并识别已经采集的内容；AI 进入思考、回答等状态时，P4 通过 UART 向 STM32 发送动作指令，控制 MG3115 和 MG995 舵机完成回中、抬头、点头等动作。

项目亮点：

- 基于 ESP32-P4 Function EV Board，使用 OpenVela/ESP-IDF 软件环境；
- 支持语音识别、DeepSeek 对话和 SiliconFlow 语音合成；
- 支持摄像头、人脸检测、行人检测和机器人眼睛表情；
- P4 与 STM32 通过自定义二进制 UART 协议通信；
- STM32 使用 TIM3 PWM 控制双舵机，并保留电机控制能力；
- API 密钥不再写入源码，改为运行时从 NVS 读取。

## 二、选题方向

AI 硬件产品创新。

本作品面向具备视觉、听觉和运动能力的桌面机器人，尝试将端侧显示、语音交互、云端大模型和底层执行机构组合成完整的自然交互设备。ESP32-P4 负责图形界面、视觉和 AI 交互，STM32 负责实时 UART 指令解析、电机和舵机控制，形成分工明确的双芯片架构。

## 三、目录结构

- `app/hello_app/` — ESP32-P4 应用工程，包含主程序、界面、语音、视觉和硬件功能组件。
- `app/hello_app/main/` — ESP32-P4 应用入口、LVGL 初始化和系统启动代码。
- `app/hello_app/components/apps/` — 机器人应用集合，包括语音交互、摄像头、眼睛表情、设置、音乐和 2048 等应用。
- `app/hello_app/components/apps/network_test/` — 语音识别、DeepSeek 对话、TTS 播放和交互状态机。
- `app/hello_app/components/robot_uart/` — P4 与 STM32 的 UART 二进制协议、心跳、电机、舵机和动作指令。
- `app/hello_app/components/api_key_store/` — API 密钥 NVS 存储接口，避免密钥直接写入源码。
- `app/hello_app/components/provisioning/` — API 密钥配置相关组件。
- `app/hello_app/components/human_face_detect/` — ESP32-P4 人脸检测模型和调用接口。
- `app/hello_app/components/pedestrian_detect/` — ESP32-P4 行人检测模型和调用接口。
- `app/hello_app/spiffs/` — SPIFFS 文件系统资源，包括界面资源和语音临时文件。
- `board/contest_board/` — openvela 板级适配目录，目前保留比赛工程的板级适配骨架。
- `quickapp/` — 快应用目录，目前未作为本作品的主要运行入口。
- `logs/` — Codex 和 OpenCode 的 AI Coding 日志，包含 `manifest.json` 和按日期归档的 JSONL 会话记录。
- `contest2026_428_zhipeihuangdi.xml` — repo 工程清单，负责映射应用、快应用和板级适配目录。

## 四、运行方式

### 1. 获取工程

```bash
repo init -u https://github.com/open-vela/contest2026_428_zhipeihuangdi \
  -b dev-ai-contest-2026 -m contest2026_428_zhipeihuangdi.xml
repo sync -c -j8
```

同步完成后，参赛仓目录为 `contest2026_428_zhipeihuangdi/`，ESP32-P4 应用位于 `app/hello_app/`。

### 2. 准备 ESP-IDF 环境

安装 ESP-IDF 5.5.x 和 ESP32-P4 对应工具链，进入应用目录并加载 ESP-IDF 环境：

```bash
cd contest2026_428_zhipeihuangdi/app/hello_app
get_idf
idf.py set-target esp32p4
```

如果使用 openvela 的完整工作区，请先按照对应教程加载 OpenVela 构建环境，再进行应用编译。

### 3. 配置和编译

```bash
idf.py menuconfig
idf.py build
```

编译前请确认 `sdkconfig.defaults` 中的目标芯片、PSRAM、摄像头、ESP-Hosted C6 Wi-Fi 和显示屏配置正确。

### 4. 配置 API 密钥

代码不会从源码头文件读取真实 API Key，而是从 NVS 的 `api_keys` 命名空间读取。烧录前应通过项目提供的配置流程写入 SiliconFlow 和 DeepSeek Key。不要把真实密钥提交到 Git 仓库、README 或 AI 日志中。

### 5. 烧录并查看日志

```bash
idf.py -p <串口号> flash monitor
```

启动后，机器人进入主界面并自动打开语音交互应用：

1. 点击麦克风按钮，界面显示“正在聆听”；
2. 对机器人说话；
3. 再次点击麦克风按钮，结束录音并识别已经采集的语音；
4. 识别成功后进入思考状态并请求 AI 回答；
5. 生成回答后播放语音，同时通过 UART 控制 STM32 舵机动作。

### 6. STM32 联动

STM32 工程使用 Keil MDK 编译，UART 参数为 `115200 8N1`：

```text
P4 GPIO7 TX  -> STM32 PA10 RX
P4 GPIO8 RX  -> STM32 PA9 TX
P4 GND       -> STM32 GND
STM32 PB5    -> MG3115 信号线
STM32 PB4    -> MG995 信号线
```

舵机电源建议使用独立 5V 电源，并与控制板共地。舵机动作由 UART 协议和语音交互状态触发：聆听时回中，思考时抬头偏转，回答时点头，异常时回中。

## 五、AI Coding 使用说明

本作品在需求分析、协议设计、代码实现、故障排查和文档整理阶段使用了 Codex 与 OpenCode 辅助开发：

- 在需求拆解阶段，分析 ESP32-P4、STM32、UART、舵机和语音交互之间的职责划分；
- 在方案设计阶段，确定 UART 帧格式、CRC 校验、舵机编号、动作命令和心跳机制；
- 在编码阶段，协助修改 P4 的 UART 组件、语音交互状态机、API 密钥存储和 STM32 的协议解析、PWM 舵机控制；
- 在调试阶段，定位蓝屏、UART 乱码、舵机引脚复用、录音停止后不上传以及 TTS 延迟等问题；
- 在文档阶段，整理工程目录、运行步骤、硬件接线和 AI 使用说明。

AI 主要帮助快速梳理跨芯片通信链路、检查协议两端的一致性、定位状态机问题，并减少了重复查找代码和整理文档的时间。所有硬件接线、烧录和实际运行结果仍需要在真实开发板上验证。

完整 Codex 和 OpenCode 对话日志见 [`logs/`](logs/) 目录。


Bilibili 演示视频：
https://www.bilibili.com/video/BV1CQez66EQK

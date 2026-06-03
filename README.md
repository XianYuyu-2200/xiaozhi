# xiaozhi ESP32-S3 Codex Pet

这是基于 `78/xiaozhi-esp32` 定制的 ESP32-S3 小智固件，当前主要适配 Movecall Moji 圆屏设备。

## 适配硬件

- 主控：ESP32-S3-N16R8
- 板子：Movecall Moji ESP32-S3 / MOJI_V1.4
- 屏幕：GC9A01 圆形 LCD
- 外设：屏幕、喇叭、麦克风

## 当前定制内容

- 黑色屏幕主题
- 蓝色文字和蓝色边框
- CLI 风格状态显示
- 每个运行状态都有动态 ASCII 表情
- 支持电脑端通过 UDP 遥控屏幕状态
- 隐藏原项目的顶部栏、聊天内容区和默认 emoji 图像
- 保留 Wi-Fi、语音、音频测试和小智基础功能

## 状态和表情

屏幕上方显示动态 ASCII 表情，下方显示当前状态文字。

| 状态 | 触发场景 | 动态表情 |
| --- | --- | --- |
| Booting | 开机/重启 | `(^_^)` / `(^.^)` |
| Codex Ready | 空闲待命 | `(o_o)` / `(-_-)` |
| Thinking | 配网、激活、连接、思考 | `(o.o)` / `(o_O)` / `(O_o)` |
| Codex Listening | 聆听/收音 | `(o_o)` / `(O_O)` |
| Codex Working | 回复、播报、处理中、升级 | `(o_O)` / `(O_o)` / `(o_o)` |
| Codex Working | Codex 正在写代码 | `(｡◕‿◕｡)` |
| Testing | 音频测试 | `(O_O)` / `(o_o)` |
| Task Complete | 一轮任务结束 | `(^_^)` / `(^o^)` |
| Error | 错误/失败 | `(T_T)` / `(x_x)` |
| Sleeping | 省电/睡眠 | `(-_-)` / `(-.-)` |

当前共有 12 个不同表情，按状态动画帧计算共有 20 帧。

## Codex 联动

固件会在 Wi-Fi 下监听 UDP `3333` 端口。电脑端发送状态指令后，屏幕会立即切换到对应表情。

支持的指令：

```text
booting
ready
thinking
listening
working
coding
testing
done
error
sleeping
```

电脑端脚本：

```text
tools/codex_moji.ps1
```

广播发送到同一局域网：

```powershell
powershell -ExecutionPolicy Bypass -File tools\codex_moji.ps1 thinking
powershell -ExecutionPolicy Bypass -File tools\codex_moji.ps1 done
```

如果知道小智的 IP，也可以指定 IP：

```powershell
powershell -ExecutionPolicy Bypass -File tools\codex_moji.ps1 thinking 192.168.1.23
```

这样就可以在 Codex 开始处理问题时发送 `thinking`，回答结束后发送 `done` 或 `ready`。

### 完全自动化

如果希望不手动执行 `codex_moji.ps1`，可以启动监听器：

```powershell
powershell -ExecutionPolicy Bypass -File tools\start_codex_moji_watcher.ps1
```

默认监听当前 Codex 线程，并把状态发送到 `192.168.0.26:3333`。如果小智 IP 变了，启动时指定新 IP：

```powershell
powershell -ExecutionPolicy Bypass -File tools\start_codex_moji_watcher.ps1 -HostName 192.168.0.26
```

监听器会读取本机 Codex 日志数据库：

```text
C:\Users\<用户名>\.codex\logs_2.sqlite
```

检测到当前线程的新用户消息时发送 `thinking`，检测到 Codex 使用 `apply_patch` 写代码时发送 `coding`，检测到当前回复完成时发送 `done`。这是基于 Codex 本地日志的外部联动方案，如果 Codex 后续版本修改日志结构，监听器可能需要同步调整。

停止监听器：

```powershell
powershell -ExecutionPolicy Bypass -File tools\stop_codex_moji_watcher.ps1
```

## 代码位置

主要定制文件：

```text
main/boards/movecall-moji-esp32s3/movecall_moji_esp32s3.cc
```

核心逻辑：

- `CustomLcdDisplay`：定制圆屏 UI
- `SetCliMode()`：切换状态
- `AnimateCliFace()`：定时切换动态表情
- `RunCliControlServer()`：监听 UDP 状态控制指令
- `ApplyStateStatus()`：把小智设备状态映射到屏幕状态

## 编译环境

推荐环境：

- Windows 10/11
- ESP-IDF 5.5.2
- Python 3.11
- ESP32-S3 USB 串口驱动

进入 ESP-IDF 环境后，在项目根目录编译：

```powershell
idf.py set-target esp32s3
python scripts\release.py movecall-moji-esp32s3
```

如果机器内存较小，可以使用低并发构建：

```powershell
cmake --build build -- -j2
```

## 烧录

设备进入下载模式后，确认串口号。例如当前测试设备是 `COM5`。

在 `build` 目录执行：

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

烧录完成后设备会自动复位。

## 使用流程

1. 第一次启动后进入配网模式。
2. 连接设备热点并完成 Wi-Fi 配网。
3. 设备联网后屏幕显示 `Codex Ready`。
4. 唤醒或开始对话后，屏幕会按状态切换动态表情。
5. 电脑端可通过 `tools/codex_moji.ps1` 让 Codex 遥控小智表情。

## 说明

这是面向 ESP32-S3 Moji 圆屏设备的个人定制版本，不保证适用于其他开发板。其他屏幕、音频 Codec 或 GPIO 排线不同的设备，需要重新适配 `main/boards` 下的板级配置。

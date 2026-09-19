# Folo AI Passport 语音视频搜索

面向 **Folo AI Passport ESP32-C3 工牌** 的语音视频搜索固件。本仓库只保留这一款硬件的板级实现，不能直接用于其他 ESP32 开发板。

本项目基于开源项目 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 2.4.2 版本适配，保留原项目 MIT 许可证。具体来源与修改范围见 [NOTICE](NOTICE)。

> English version: [README.en.md](README.en.md)

## 后端服务依赖

本固件的语音视频搜索功能依赖独立的后端项目 [westwind027/video-semantic-search](https://github.com/westwind027/video-semantic-search)。该项目负责视频解析、场景索引、语义检索和预览图片 URL 提供；本固件负责语音采集、通过小智协议获取 ASR 结果、调用搜索接口以及在设备屏幕上展示结果。

固件默认调用场景搜索接口：

```text
GET /v1/search?q=<URL 编码后的查询词>&limit=12&mode=scene
```

返回结果需要包含可由设备访问的预览图片地址。固件优先读取 `image_url`、`preview_url`，并兼容后端场景结果中的 `scene.preview`；相对路径会自动拼接服务地址。后端的 `/static/frames/...` 支持 `size=small` 和 `size=tiny` 图片变体，固件分别用于全屏查看和结果缩略图。

## 已支持功能

- ESP32-C3，8 MB Flash，无 PSRAM
- 240 × 320 ST7789P3 SPI 屏幕及 PWM 背光
- ES8311 编解码器、麦克风输入和扬声器输出
- GPIO0 ADC 电阻梯三键
- 设备端 Wi-Fi 扫描、选择和密码输入
- 通过小智协议获取 ASR 结果（保留连接和对码逻辑，不显示小智对话页）
- 视频搜索服务查询和结果图片展示
- 简体中文资源和“你好小智”本地唤醒词
- WebSocket、MQTT/UDP 与设备端 MCP 能力

## 硬件连接

| 功能 | GPIO / 参数 |
| --- | --- |
| LCD MOSI / SCLK / CS / DC | GPIO9 / GPIO8 / GPIO1 / GPIO20 |
| LCD RST / 背光 | 未连接 / GPIO21 |
| ES8311 I²C SDA / SCL | GPIO10 / GPIO7 |
| I²S MCLK / BCLK / WS | GPIO6 / GPIO5 / GPIO3 |
| I²S DOUT / DIN | GPIO2 / GPIO4 |
| 三键 ADC | GPIO0 / ADC1_CH0 |
| USB Serial/JTAG | GPIO18 / GPIO19 |

按键默认行为：主页面空闲时上键音量 `+5`、下键音量 `-5`，确认键开始或停止录音；确认键长按约 850ms 进入设置。搜索结果页上/下切换结果，长按上向服务端发送远程播放命令（自动打开当前视频并跳到场景时间，POST `/v1/player/control`），长按下关闭远程播放（`{"action":"close"}`）；遥控请求与搜索/图片传输共用同一个 HTTP worker（忙时提示“设备忙”），避免并发挤占内存；确认键开始新搜索，长按仍进入设置。设置页用上/下键选择菜单项，OK 进入；进入音量页后上/下调节音量、OK 返回。

搜索结果主页面请求服务端的 `?size=tiny&width=80&height=60` 图片，全屏查看请求服务端原生的 `?size=small` 图片。主页面使用首次申请后持续复用的 12 KB JPEG 接收缓冲和 96×64 RGB565 预览缓冲；全屏页面不创建整屏 RGB565 缓冲，而是使用同一块可复用的 JPEG block 缓冲，逐个 MCU 行块直接刷入 LCD。显示端会按屏幕比例缩放，并支持水平/垂直翻转，配置项位于板卡 `config.h`。

搜索接口默认请求 12 条结果；若响应超过 16 KB 或结果解析分配失败，固件会释放本次请求资源并自动按 `12 → 6 → 3 → 1` 降级重试，优先显示较少结果，避免触发未处理的 `Search task ran out of memory`。

首次启动或没有已保存 Wi-Fi 时，设备直接显示附近 Wi-Fi 列表：上/下选择网络，OK 打开密码键盘，`GO` 提交连接，OK 长按返回设置。键盘布局和快捷操作与 [leo-radio](https://github.com/leo0183/leo-radio) 一致：6 列字符网格、大小写/数字页切换、`<` 删除、`GO` 提交；短按上/下移动一个按键，长按上/下跨一整行。

设置中的“服务地址配置”使用同一套字符键盘，默认值为公开发布安全的占位地址 `http://video-search.local:8000`；请在设备菜单中改成实际服务地址，选中 `GO` 保存，OK 长按取消并返回设置。所有菜单子页面均使用 OK 长按返回上一级；应用不启动旧的热点网页配网或搜索配置网页。

## 构建

本项目固定使用 ESP-IDF 5.5.5；不要切换到其他 IDF 版本。该版本已完成本板构建和实机烧录验证。

```sh
python scripts/build.py folo/ai-passport-c3 \
  --name folo-ai-passport-c3 \
  --language zh-CN \
  --wake-word nihaoxiaozhi
```

构建完成后，完整 8 MB 合并镜像位于 `build/merged-binary.bin`。

## 烧录

把 `PORT` 替换为实际串口：

```sh
python -m esptool --chip esp32c3 --port PORT --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build/merged-binary.bin
```

烧录会覆盖 Flash 中原有固件及运行数据。首次启动后直接在设备屏幕选择 Wi-Fi 并输入密码。

## 目录

- `main/boards/folo/ai-passport-c3/`：工牌引脚、屏幕、音频和按键初始化
- `main/boards/common/`：小智公共板卡抽象和 Wi-Fi 支持
- `main/audio/`：录音、播放、编解码和唤醒词
- `main/display/`：LCD/LVGL 界面
- `main/protocols/`：WebSocket 与 MQTT/UDP 协议
- `scripts/build.py`：唯一推荐的构建入口

## 当前限制

- 若 CW2017 不应答或 SOC 尚未完成首次计算，顶部电池图标会暂时隐藏；其余功能不受影响。
- 已验证编译、Flash 内容以及基础启动/配网；麦克风、扬声器、按键和长期稳定性仍建议在不同批次硬件上继续测试。

## 许可证

本项目采用 [MIT License](LICENSE)。发布衍生版本时请保留 `LICENSE` 和 `NOTICE`。

# ESP32-C3 内存分配问题排查与方案验证记录（handoff）

适用范围：`folo/ai-passport-c3` 单板（ESP32-C3，无 PSRAM，8MB Flash，ESP-IDF 5.5.5）。
本文整理搜索/图片/遥控链路内存问题的根因、已验证的方案与结果、当前代码状态，以及后续可走的路线。

> **2026-09-14 更新：** §3.3、§3.7、§3.8 中“内存紧张时释放解码区再申请”的方案已被
> 实机串口日志证伪。当前根治设计及实施验收以
> [search-memory-fix-plan.md](search-memory-fix-plan.md) 为准；本文以下内容保留为故障演进记录，
> 不再代表最终架构。

---

## 1. 环境与内存账本

- C3 的 SRAM 固定 ~321KB，**分区表只影响 Flash，调不出 RAM**（`partitions/v2/8m.csv`：ota_0/ota_1 各 3MB + assets 2MB，健康，无需调整）。
- 静态 DRAM（`idf_size.py --archives build/xiaozhi.map` 实测）：约 113KB（.text in DRAM ~69.7KB + .bss 26.5KB + .data 16.8KB；esp-sr 摘除前 .data 为 22.8KB，其中 `libdl_lib.a` 占 5936B，见 §3.11）。
- **RAM 中的 .text（.iram0.text，69810B）已逐库归因，无压缩空间**：这是必须驻留 RAM 的代码
  （Flash cache 不可用时也要能执行：中断向量/ISR、Flash 读写、调度器、堆、RF PHY），
  无法"改为引用 Flash"。分布：FreeRTOS 14.2KB、hal 9.7KB、spi_flash 9.4KB、esp_hw_support 7.7KB、
  heap 7.4KB、SPI 驱动 4.5KB（LCD 用）、phy 4.5KB、其余为各类驱动 ISR；
  **应用层（libmain.a）为 0 字节**，WiFi 相关（pp+phy+esp_wifi）仅 ~5.7KB 且为 RF 必需。
  注意：占大头的是 `.flash.text` 1.64MB，那部分本就从 Flash 缓存执行、不占 RAM。
- `.bss`（26552B）与 `.data`（22772B）已逐库归因：
  - `.bss` 大头：net80211 7.6KB + wpa/pp/smartconfig ~2.6KB（WiFi 驱动静态缓冲，固定）、
    libstdc++ 4.5KB（`locale_init.o` 的 std::locale 静态数据——被 esp-ml307 的
    HttpClient 实际使用 `ostringstream/istringstream` 拉入，第三方不改就无法移除）、
    freertos 2.6KB、lwip 2.5KB（池参数可微调几百字节，收益小）；应用层 libmain 仅 1.9KB。
  - `.data` 大头（摘除 esp-sr 后）：hal 5.5KB、esp_new_jpeg 3.3KB、pp/phy/spi_flash
    等驱动表格，固定；应用层仅 36B。esp-sr 的 libdl_lib 5936B 已随第二批摘除回收
    （.data 22772 → 16836，堆上限上移 ~5.8KB，另回收 flash 侧 wakenet/dl_lib
    代码 ~41KB，xiaozhi.bin 从 2.75MB 缩到 2.51MB）。
  - 另：`.dram0.dummy` 69632B 是链接脚本静态预留的堆区本体，非浪费。
- 主要任务栈：opus_codec 24KB（最大单点）、main 8KB、audio_input 6KB、audio_output 4KB、video_search worker 4KB、search_image 8KB、tcp_receive 4KB（esp-ml307 每连接从堆分配）。
- 编译期已裁剪：`CONFIG_WAKE_WORD_DISABLED=y`（业务流程为按键→说话→ASR，本地唤醒词从未使用），空闲堆因此 +30KB。
- `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=8`（见 §3.2）。
- 音频引擎：C3 恒用 LiteAudioEngine（AfeAudioEngine 仅 S3/P4），AFE/AudioProcessor 在 C3 上本就未运行；录音结束为按键手动（`kListeningModeManualStop`），VAD 只驱动 LED。

关键认知：**空闲堆 ~40-50KB 是正常水位**；问题从来不是"起始内存低"，而是几个动态消耗源（wifi TX 池、codec input 链路、HTTP/TCP 连接临时内存、任务栈）在搜索窗口叠加。

---

## 2. 崩溃根因：esp-ml307 TCP 任务 bad_alloc → abort 重启

### 2.1 现象
搜索/图片阶段随机重启，无 Backtrace 行，签名：
`abort() was called at PC 0x4214e6bd on core 0`（不同构建 PC 低位不同）+ `rst:0xc (RTC_SW_CPU_RST)`。

### 2.2 定位方法
崩溃时 ESP-IDF 会打印 Stack memory 段；从栈内存中提取返回地址（RA）序列，
用 `riscv32-esp-elf-addr2line -pfiaC -e build/xiaozhi.elf <PC> <RA...>` 符号化，得到调用链：

```
EspTcp::ReceiveTask (esp_tcp.cc:65)
 → HttpClient::OnTcpData (http_client.cc:277)
 → ProcessReceivedData (http_client.cc:374)
 → ParseRegularBody (http_client.cc:508)
 → AddBodyData (http_client.cc:722)
 → deque<DataChunk>::emplace_back → string 拷贝 → operator new
 → std::bad_alloc（TCP 任务内未捕获）→ terminate → abort
```

### 2.3 机制
esp-ml307（`managed_components/78__esp-ml307`，不可手改）的 TCP 接收任务在堆上用
`operator new` 分配 HTTP body chunk（`EspTcp::ReceiveTask` 里 `data.resize(1500)`，每次 ≤1.5KB）。
堆耗尽时抛出未捕获的 `std::bad_alloc` → 整机 abort。**崩溃点在第三方组件任务里，业务层无法 catch。**

### 2.4 隐藏成本（实测确认，容易低估）
每次 HTTP 连接在第一批数据到达前，堆上还要付出：
- 新建的 `tcp_receive` 任务栈 4KB（`xTaskCreate` 在 `EspTcp::Connect` 里，从堆分配）；
- LWIP 连接控制块等 ~1KB。

即图片下载的真实起步需求 ≈ **14KB 接收 buffer + ~5KB 连接建立 + ~2KB chunks**。
曾按"接收 + 1.5KB chunk"估算门控余量（4KB），实际 largest=19456 时门控放行、
连接建立后剩 ~5KB，仍打崩 TCP 任务——这是 2026-09-13 那次复现的直接原因。

### 2.5 esp-ml307 被动断开事件组竞态（第二类崩溃，2026-09-13 发现）
签名：`assert failed: xEventGroupSetBits event_groups.c:561 (xEventGroup)`，无 Backtrace。
链路：服务端先 FIN（搜索响应被截断 `Connection closed prematurely`，或 close-delimited
响应结束）→ `tcp_receive` 任务在 `DoDisconnect(false)` 里把 `connected_` 置 false 后
退出循环 → 主线程同刻看到 EOF，`Close()` 因 `connected_==false` 跳过等待、
`~EspTcp` 直接 `vEventGroupDelete` → 任务包装函数随后对已删除句柄
`xEventGroupSetBits` → assert 重启。主动关闭路径（`DoDisconnect(true)` 等 exit bit）是安全的。
**规避**：所有 HttpClient 退役统一走 `RetireHttpClient()`（Close → 100ms 宽限让任务
完成 SetBits → 销毁），覆盖搜索/图片/遥控全部分支；由回归测试钉住。

---

## 3. 已验证方案与结果（按时间线）

### 3.1 编译期禁用唤醒词 ✅ 有效
`main/boards/folo/ai-passport-c3/config.json`: `CONFIG_WAKE_WORD_DISABLED=y`。
空闲堆 42KB → 72.5KB（+30KB）。业务无感。

### 3.2 WiFi 动态 TX 缓冲池限流 ✅ 有效
默认 `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32`（满配 ~51KB），说话上传 + HTTP 时按需扩张
且**扩张后常驻不归还**（实测扩到 ~14 个占 22KB）。config.json 限为 8（上限 12.8KB）。
教训：无 PSRAM 设备上任何动态扩张的内存池都要关注"扩张后是否归还"。

### 3.3 图片管线 buffer 开机预分配钉死 ✅ 有效（当前架构）
`PreallocateImageBuffers()` 在开机堆最干净时一次性分配：
- 接收 buffer 14KB（`kImageReceiveBufferBytes`）
- 解码 buffer ~10.75KB（`kMaxDecodeBufferBytes + 512B` 余量，`kDecodeBufferAllocBytes`）

**为什么必须钉死**："释放后重分配同尺寸"是确定性故障——malloc 头开销使重分配需要
N + overhead 的连续块，而释放后的块恰好是 N（曾实测 `largest=10240` 恒定、解码 buffer
需 10752B 必然失败）。运行期禁止 `ReleaseImageBuffers()` 全释放；接收 buffer 现为
**全程钉死（含搜索期，见 §3.10）**，解码 buffer 仅在真实内存预算失败时经重试逻辑释放。

### 3.4 搜索开始立即关闭 codec input ✅ 有效
原逻辑靠 idle power timer 25s 无活动才 `EnableInput(false)`，搜索词获取后 input 链路
（I2S DMA + codec）仍占着内存贯穿整个搜索窗口。
`AudioService::RequestInputStop()`（置 `AS_EVENT_AUDIO_INPUT_STOP_REQUEST`，input task
重查 active bits 后安全关闭）在 `Application::StartSearch` 中调用。
实测：`Search start` 时 input 已关，free 明显变大。

### 3.5 遥控并入共享 worker ✅ 有效
早期遥控用独立 4KB 任务 + 私有 HttpClient 与图片管线并发抢堆。改为 PlayerControlClient
纯门面，POST 走 VideoSearchClient 共享 worker 串行化。

### 3.6 opus_codec 任务栈裁剪 ❌ 失败，已回滚（重要教训）
- **24KB → 12KB**：开麦瞬间 `Stack protection fault`，canary 显示 SP 越过下界 ~3.2KB，
  addr2line 定位在 opus CELT 编码器深处（`celt_encode_with_ec → run_prefilter`）。
- **24KB → 20KB**：没崩但 `opus_codec stack high water: 772 bytes`（只剩 772B），说话时
  实测峰值用量 **~19.7KB**——纯属侥幸。
- **结论：24KB 是真实需求，不可裁**。任务内保留 30s 节流的
  `uxTaskGetStackHighWaterMark` 日志（`opus_codec stack high water: <剩余字节>`），
  任何再裁剪的尝试都以它为准。

### 3.7 图片下载 TCP 余量门控 ✅ 有效（迭代三轮）
`EnsureImageReceiveBufferWithHeadroom()`：重钉接收 buffer 前检查堆余量，不够则先释放
闲置解码 buffer 再试，仍不够**干净跳过该图**（宁可无图不可重启）。
余量常量 `kImageTcpHeadroomBytes` 迭代：
- 4KB：低估了连接建立成本（§2.4），largest=19456 时放行仍崩 → **8KB**；
- 门控逻辑 bug：接收 buffer 已钉着时（上一张图下载后未释放，设计内），仍按
  "14KB + 8KB"检查最大块，堆碎片化后最大块 18.4KB → 连续 7 张翻页图被误判跳过
  → 已修：**已钉着时只检查 ≥8KB 余量**。

### 3.8 搜索分支同款预检 ✅ 已加
搜索 JSON body 也经 esp-ml307 TCP 任务分块。若全部钉死状态下 largest < 8KB，
先释放解码 buffer 再发请求（否则中途 bad_alloc = 直接重启而非可重试失败）。

### 3.10 搜索期归还接收 buffer ❌ 废除（第一张图必失败根因）
早期设计：搜索期归还接收 buffer 给 JSON body，图片期重分配。实测后果：
**每次搜索后的第一张图必定失败**——接收 buffer 重分配恰好从合并区拿走 14336B，
把解码块孤立成精确 10752B，下载成功后解码重分配请求 10752B 差几个 malloc 头字节失败
（日志：`largest=10752` + `Failed to allocate reusable search image decode buffer`）；
第二张图起堆重新合并到 22528 就正常——与"翻页都成功"完全吻合。
修复：接收 buffer 全程钉死；JSON body（~6-9KB）在 largest≈11.8KB 下够用，
万一超预算由既有 `bad_alloc → 释放解码重试 → 降额` 链接管。

### 3.11 esp-sr 摘除 ✅ 已实施（第二批第一项）
前提确认：C3 恒为 `WAKE_WORD_DISABLED`、v2 分区表无 model 分区、生成的 assets 索引无
`srmodels` 键 —— wakenet **从未在运行时加载**，链接进去纯属死重。
实施内容：
- `main/idf_component.yml` 删除 `espressif/esp-sr`（组件管理器自动清理
  `managed_components/espressif__esp-sr`）；
- `AudioEngine/WakeWord::Initialize` 签名去掉 `srmodel_list_t*` 参数，删除
  `AudioService::SetModelsList`、`Assets::LoadSrmodelsFromIndex` 及 `model_path.h` 引用；
- `lite_audio_engine` 改持 `unique_ptr<WakeWord>`，EspWakeWord 创建处用
  `#if CONFIG_USE_ESP_WAKE_WORD` 包裹（若未来恢复本地唤醒词仍可编译）；
- `main/CMakeLists.txt`：`esp_wake_word.cc` 仅在未禁用唤醒词时编译；不再传
  `--esp_sr_model_path` 给 assets 脚本；
- `sdkconfig.defaults.esp32c3` 删 `CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y`；
- `scripts/build.py`：lite 目标（esp32c3 等）传非 disabled 的 `--wake-word` 时降级为
  disabled 并告警（历史命令兼容，AGENTS.md 验证命令不需改）。
收益（map 实测）：`.data` 22772 → 16836（**-5936B**，精确等于 libdl_lib）、堆上限上移
~5.8KB、flash 侧 wakenet/dl_lib 代码 ~41KB 回收、xiaozhi.bin 2.75MB → 2.51MB。

### 3.9 规范层面不采纳的方案
- **调整分区表换 RAM**：概念性误解，分区表只影响 Flash。
- **裁剪 WebSocket/MCP server**：只省 Flash 不省 RAM。
- **esp-sr 摘除**（libwakenet 22.8KB flash + libdl_lib ~6KB RAM + srmodels.bin assets）：
  ~~列为待办~~ → **已实施，见 §3.11**。
- **运行期反复释放/重分配大 buffer**：见 §3.3，确定性失败，禁止。

---

## 4. 当前状态（2026-09-13）

- 最新固件新增两笔待复测记录：
  ①`RetireHttpClient` 修复 §2.5 事件组竞态（此前日志中出现
  `assert failed: xEventGroupSetBits`，触发序列 = 搜索响应被截断 + 首图下载）；
  ②esp-sr 摘除（§3.11），堆上限上移 ~5.8KB。
- 上一版（搜索期全程钉死接收 buffer）曾验证：多轮"说话 → 搜索 → 翻页图片 → 长按遥控"
  全程无重启无分配失败；但当时未察觉构建实际失败、烧录的是更旧固件——**复测须以本次
  烧录为准**。
- opus_codec 高水位余量 ~4.8KB（24KB 栈 − ~19.7KB 峰值）。
- 回归测试：`python -m unittest discover -s scripts/tests`（33 项）全过，
  含对以上所有机制的 pin 测试。
- 相关代码位置：
  - `main/search/video_search_client.{h,cc}`：PreallocateImageBuffers、
    EnsureImageReceiveBufferWithHeadroom、kImageTcpHeadroomBytes、搜索分支预检
  - `main/audio/audio_service.{h,cc}`：RequestInputStop、opus 栈与高水位日志
  - `main/application.cc`：StartSearch 中调用 RequestInputStop
  - `main/boards/folo/ai-passport-c3/config.json`：wifi TX=8、wake word disabled

### 待办（按需推进）
1. TTS/opus 解码路径评估裁剪（用户表态：TTS 可不要；但 UI 音效也走解码，需替代方案）。
2. `minimal sram ~5.7KB` 的峰值来源持续观察（图片解码 + 显示叠加窗口）；esp-sr 摘除后
   预期整体水位上移 ~5.8KB。
3. 遥控/搜索路径若再遇 esp-ml307 第三方缺陷，评估 vendor patch。

---

## 5. 诊断手册

### 5.1 抓包与日志关键字
```
Select-String -Path build\serial_log.txt -Pattern "Guru|abort|Stack protection|rst:|Failed|high water|Search start|worker op|Released the decode|Streamed"
```
- `worker op=N start/done: heap free=… largest=…`：搜索 worker 各操作前后的堆。
- `Search start: heap free=… largest=…`：搜索发起时刻（应已关 codec input）。
- `opus_codec stack high water: N`：N 为**剩余**字节（24KB − 峰值用量）。
- `Released the decode buffer …`：门控释放了解码 buffer（正常但应低频出现；若每次
  图片都出现且伴随跳图，说明门控判定又出问题）。
- `minimal sram`：峰值压力优先看这个值。

### 5.2 崩溃符号化
```sh
# 崩溃日志中取 MEPC 与 Stack memory 段里的 RA 值序列
riscv32-esp-elf-addr2line -pfiaC -e build/xiaozhi.elf <PC> <RA1> <RA2> ...
```
无 Backtrace 行的 abort 多为第三方任务内未捕获异常——栈内存里的 RA 序列是唯一线索。

### 5.3 静态内存盘点
```sh
python "$IDF_PATH/tools/idf_size.py" --archives build/xiaozhi.map
```
`.iram0.text` 的逐库归因方法：map 文件中 `.iram0.text` 输出段内，条目有两种格式——
"独立段头行 + `addr size obj` 行"（逐一归档计数），以及 esp_wifi/esp_phy 等
"段名+addr+size+路径"单行格式（按段累加）；二者合计应恰好等于段头标注的总尺寸
（本仓验证：69810B，与 idf_size 的 DRAM .text 一致）。

### 5.4 构建烧录
按本地 ESP-IDF 5.5.5 工具链设置 `ESP_ROM_ELF_DIR`；串口抓取和构建日志只保留在本地，
不要提交到公共仓库。

---

## 6. 经验总结（给后来者）

1. **崩溃先问"谁在分配"，别急着加 buffer**：abort 链的终点是 `operator new`，
   但触发者是整条内存预算链（wifi 池、codec input、连接建立成本）。
2. **门控余量要算上"连接建立"这类一次性成本**（每连接 4KB 任务栈 + LWIP），
   只按稳态 chunk 估算会差一倍。
3. **门控条件必须区分"需要分配"和"已经钉着"**：对已钉住的 buffer 按
   分配需求检查最大块，会在堆碎片化后产生系统性误判（表现为功能失效而非崩溃）。
4. **任务栈裁剪必须以实测高水位为准**，估算值在 opus CELT 这种深调用链上能差 60%+。
5. **优雅降级优于崩溃**：图片路径宁可跳图，也不给 esp-ml307 的 TCP 任务留下
   分配失败的机会——那个任务里没有 try/catch，失败即整机重启。

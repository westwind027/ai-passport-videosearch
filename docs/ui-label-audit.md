# 固件 UI 文案审计

## 结论

`//` 在原型中用于赛博装饰分隔，但在 240×320 实机上会占用宝贵的横向空间，且
`CORE // A2:7F`、`HEAP: FIXED`、`IDF` 等内容对用户没有操作价值。固件显示层现在不再
输出装饰性的 `//` 文案；实际的 `http://`、`https://` 地址仍保留，因为它们是用户正在
编辑的配置数据。

## 文案清单

| 原显示 | 当前显示 | 处理理由 |
| --- | --- | --- |
| `CORE // A2:7F` | `DEVICE READY` | 删除伪设备标识，保留待命状态 |
| `SEARCH // V2` | `VOICE SEARCH` | 删除无意义版本号，说明当前功能 |
| `HEAP: FIXED // CODEC: OPUS // NET: READY` | `WIFI READY` | 内存和编解码器属于调试信息 |
| `MIC // ES8311` | `VOICE INPUT` | 使用功能名称，不暴露芯片型号 |
| `CODEC: OPUS // SAMPLE: 16kHz // MIC: ES8311` | `VOICE INPUT ACTIVE` | 录音页只提示当前动作 |
| `UPLINK // 2.4GHz`、`RADAR // 2.4GHz` | `WIFI LINK`、`WIFI LIST` | 频段是重复的实现细节 |
| `SECURITY // TOKEN` | `PAIRING` | 直接说明正在等待对码 |
| `TOKEN: PENDING // ACT: READY` | `WAITING FOR PAIRING` | 合并成用户可理解的状态 |
| `KERNEL // SETUP` | `SETTINGS` | “内核”不是设备设置的用户概念 |
| `FLASH / IDF / NVS` 遥测组合 | `DEVICE SETTINGS` | 移除无操作价值的硬件信息 |
| `INPUT // MATRIX` | `TEXT INPUT` | 说明页面用途 |
| `SEARCH // ERROR` | `SEARCH ERROR` | 保留错误含义，去掉装饰分隔符 |
| `QUERY // VIDEO` | `VIDEO SEARCH` | 说明当前搜索对象 |
| `MODE: SCENE // LIMIT: 12 // CACHE: FIXED` | `VIDEO SEARCH · MAX 12` | 只保留用户需要知道的结果上限 |
| `SORT: RSSI_DESC // MODE: STATION` | `SORTED BY SIGNAL` | 将实现字段改成可读提示 |
| `AUDIO // OUTPUT`、`CODEC: ES8311 // STORAGE: NVS` | `AUDIO OUTPUT`、`VOLUME SAVED` | 显示操作结果，不显示芯片和存储实现 |
| `PASSWORD // MATRIX`、`HTTP // ENDPOINT` | `PASSWORD EDIT`、`SERVICE URL` | 明确输入类型 |
| `HOLD UP/DN: JUMP 6 // OK: INPUT` | `HOLD U/D = ROW` | 压缩键盘导航提示，底部按键说明负责具体操作 |

## 尺寸约束

- 顶部标签槽扩大为 28×16，标签统一使用 `AI`、`REC`、`NET`、`PIN`、`CFG`、`AP`、`KEY`、`ERR`、`SRC`、`VOL`、`SVR` 等短标识；应用名槽调整为 80 像素。
- 顶部副栏左右各 112 像素，文案统一使用短 ASCII 标签；超长文本采用 `LV_LABEL_LONG_DOT`，不再横向滚动或静默显示半截。
- 底部状态条从 14 像素提高到 18 像素，避免 14 像素字体被上下裁切；状态文字同样使用省略显示。
- 两个底部按键单元各 120 像素，按键徽标从 30 像素增加到 34 像素，`HOLD` 不再被挤掉；说明文字保留 76 像素并使用省略显示。
- 设置列表行调整为 30 像素高、3 像素间隔，六行列表在状态条上方完整结束，不会被底部状态条覆盖。
- Wi-Fi 名称、RSSI、键盘上下文和服务地址均限制在自己的槽位内。超长 SSID 或地址显示省略号，不覆盖状态栏。
- `docs/ui-mockups/cyberpunk-new.html` 中的设备界面文案已同步；文档标题、源码注释和真正的 URL 不属于设备显示文案。

自动化检查位于 `scripts/tests/test_search_regressions.py` 的
`test_cyber_ui_does_not_render_decorative_double_slash_labels`，用于防止新的固件显示
文案重新引入装饰性 `//`。

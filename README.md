# Crypto Taskbar

在 Windows 任务栏显示币价：每次显示一个币种，按顺序向上滚动切换；鼠标停留时展开完整列表。独立运行，不需要 Windhawk。

<img src="docs/dark.png" width="360" alt="深色行情列表，示意数据" />
<img src="docs/light.png" width="360" alt="浅色行情列表，示意数据" />

## 使用

从 [Releases](https://github.com/simulacraliasing/crypto-taskbar/releases/latest) 下载 `CryptoTaskbar.exe`，双击运行。默认轮播 BTC、ETH、SOL，每 4 秒切换，每 30 秒更新行情。

- 鼠标停留约 0.3 秒：展开完整列表；移开后收起。移到列表上可以继续查看，长列表支持滚轮。
- 点击币价：固定或收起列表。停留查看或打开配置时，轮播暂停。
- 在币价显示区域滚动鼠标滚轮：手动切换币种。
- 在左下角整块显示区域右键：配置、刷新、开机启动、退出。空白处同样可以右键。
- 运行时不显示任务栏应用图标，也不显示托盘图标；开机启动默认关闭。

文字和完整列表跟随 Windows 的系统/任务栏明暗主题。程序不会更改 Windows 的主题设置。

## 图形配置

右键 → **配置…**，通过三个页面设置，无需编辑文本文件。

**币种**：左边是轮播顺序，右边填写显示名称、币种代码并选择数据源；可以添加、更新、删除及上下移动。最多支持 100 个币种。

**数据源**：内置 CoinGecko、Binance、OKX；可以设置 URL、价格路径、24 小时涨跌幅或开盘价路径，以及可选请求头。数据源重命名时会同步更新币种引用；正在使用的数据源不能直接删除。

**显示**：调整行情刷新间隔、轮播间隔、宽度、位置、边缘距离、价格前缀、主题和涨跌幅显示。保存后立即生效，旧配置自动备份。

| 设置 | 默认值 | 范围或说明 |
|---|---|---|
| 行情刷新 | 30 秒 | 5–86400 秒 |
| 轮播间隔 | 4 秒 | 1–3600 秒 |
| 显示宽度 | 150 | 80–1200，按 100% 缩放计 |
| 位置 | 任务栏左侧 | 也可放到系统托盘左侧 |
| 边缘距离 | 16 | 按 100% 缩放计 |
| 主题 | 跟随系统 | 也可指定浅色或深色 |
| 价格前缀 | `$` | 只改变显示，不换算报价 |
| 任务栏涨跌幅 | 关闭 | 悬停列表始终显示可用的涨跌幅 |

CoinGecko 的币种代码如 `bitcoin`、`ethereum`；Binance 如 `BTCUSDT`、`ETHUSDT`；OKX 如 `BTC-USDT`、`ETH-USDT`。界面会显示相应示例。不同数据源的报价单位可能是 USD 或 USDT。

### 自定义数据源

支持返回 JSON 的 HTTP/HTTPS GET 接口。路径采用 [JSON Pointer](https://json.nlohmann.me/features/json_pointer/)，如 `/data/0/last` 表示 `data` 数组第一项的 `last` 字段。

| 输入项 | 示例 |
|---|---|
| 名称 | `my-api` |
| 接口 URL | `https://example.com/ticker?symbol={symbol}` |
| 价格路径 | `/data/price` |
| 涨跌幅路径 | `/data/change24h`，可留空 |
| 请求头 | `Authorization: Bearer %PRICE_API_TOKEN%`，可留空，每行一项 |

- URL 中的 `{symbol}` 会替换并转义为当前币种代码，路径中也支持这个占位符。
- `{ids}` 会替换成使用同一数据源的所有币代码，以逗号分隔。相同数据源、相同 URL 的请求会合并；默认 CoinGecko 列表每轮只发一次请求。
- 价格可以是数字或数字字符串。涨跌幅字段按百分数读取，`2.5` 表示 `+2.50%`。
- 只有开盘价时填写“24 小时开盘价路径”，程序按 `(当前价格 / 开盘价 - 1) × 100` 计算涨跌幅。
- 请求头支持 Windows `%环境变量%`，设置环境变量后重启程序。个人凭据不应放进源码或仓库。

读取失败时保留上次价格，显示旧数据和失败原因。HTTP 429 后至少等待 60 秒再尝试该数据源。程序不会把响应正文或认证头写入日志，也不跟随 HTTP 重定向。

配置内部保存在 `%LOCALAPPDATA%\CryptoTaskbar\config.json`，上一版保留为 `config.json.bak`。[示例文件](config.example.json)供备份、迁移和开发使用，日常通过界面配置即可。

## 任务栏适配

支持主显示器底部横向任务栏，随缩放和分辨率变化重新定位，任务栏自动隐藏或全屏应用运行时隐藏。已在 Windows 11 Build 26300 编译和检查；Windows 10 和第三方任务栏尚未验证。

程序显示在任务栏空白区域，不自动移动开始按钮、天气或应用图标。如果发生遮挡，可在图形配置中调整位置、边缘距离或宽度。应用图标靠左时，通常选择“系统托盘左侧”更合适。

程序通过系统代理设置访问行情接口，不需要交易所登录或交易权限。接口可能缓存或限流，刷新间隔不等于逐笔行情推送。

## 构建

需要 CMake 3.20+ 和支持 C++17 的 Windows 编译器。第三方 JSON 头文件已随源码提供，构建不需要下载依赖。

Visual Studio 2022：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

MinGW-w64 + Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`network_tests` 是手动运行的真实接口检查，不放入默认离线测试：

```powershell
cmake --build build --config Release --target network_tests
```

参数：`--config "路径\config.json"` 指定单独配置，`--settings` 启动时打开图形配置。`--preview-light` / `--preview-dark` 使用示意行情，方便检查配色；不更改系统主题，也不显示任务栏或托盘图标。

MIT License。第三方许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。验证范围见 [docs/validation.md](docs/validation.md)。
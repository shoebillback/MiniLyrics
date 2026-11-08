# MiniLyrics

Windows 原生桌面歌词小窗，基于 BetterNCM、RefinedNowPlayingNext 与 InfLink-rs。当前版本 **0.6.17**。

显示端使用 C++ / Win32 / Direct2D / DirectWrite，JavaScript 负责歌词、播放信息及控制桥接。运行时无需安装 .NET、Node.js 或 WebView2。

## 功能

- 普通、逐字歌词与翻译；换句、点击跳转和前奏动画。
- 窗口置顶、鼠标穿透、拖动与缩放，多 DPI 屏幕适配。
- 滚轮浏览歌词，停止操作约四秒后回到播放位置。
- 封面、进度与播放控制，封面配色及 Lyricify 动态背景。

## 安装

需要 Windows 10/11 x64、网易云音乐 3.1.11+、BetterNCM ≥ 1.0.0、RefinedNowPlayingNext 与 InfLink-rs。历史接口核对版本为 RNPNext 3.0.2+locallyrics.2、InfLink-rs 3.4.0；其他版本需验证。

1. 下载 [MiniLyrics-0.6.17.plugin](https://github.com/shoebillback/MiniLyrics/raw/refs/heads/main/dist/MiniLyrics-0.6.17.plugin)。
2. 完全退出网易云音乐与旧 MiniLyrics 小窗，将插件包放入 BetterNCM 数据目录下的 `plugins` 文件夹，替换旧包。
3. 重启网易云，在 MiniLyrics 设置中点击“打开 / 找回小窗”。

升级时应同时更新脚本和原生显示端，直接使用完整插件包即可。

## 项目结构

| 路径 | 用途 |
| --- | --- |
| `core.js` / `plugin.js` | 歌词数据处理与 BetterNCM 桥接 |
| `native/` | C++ 原生窗口、绘制、动画和背景 |
| `tests/` | JavaScript、C++ 与 Windows 集成测试 |
| `build.ps1` | 编译原生显示端并生成插件包 |
| `main.js` / `MiniLyricsHost.exe` | 随版本提供的运行文件 |
| `dist/` | 当前版本的插件安装包 |

## 构建

Windows 下安装 MinGW-w64 g++（需支持 Direct3D 11 / Direct2D）与 PowerShell，将 g++ 加入 PATH。JavaScript 测试需要 Node.js 24+。

```powershell
powershell -NoProfile -File .\build.ps1 -OutputDirectory .\dist
```

默认输出到 `dist/`。插件包同时附带完整源码、测试及许可证。

仅重新生成脚本及插件包，使用随仓库提供的原生程序：

```powershell
powershell -NoProfile -File .\build.ps1 -SkipNativeBuild -OutputDirectory .\dist
```

## 测试

不启动网易云的 JavaScript 数据与桥接检查：

```powershell
node --test .\tests\core.test.cjs .\tests\bridge.test.cjs .\tests\background.test.cjs .\tests\update-flow.test.cjs
```

Windows 原生专项会编译独立测试程序：

```powershell
powershell -NoProfile -File .\tests\native.ps1 -Focus cleanup -ResultDirectory "$env:TEMP\MiniLyrics-native-tests"
```

其他专项见 `tests/`；`native.ps1 -Focus all` 运行默认原生回归，专项名称见脚本的 `Focus` 参数。测试输出放在 `work/` 或指定目录，不提交到仓库。独立测试不等于真实网易云会话联调。

## 版本与许可证

0.6.17 精简重复状态、绘制与消息处理，保留未变化的歌词排版缓存。详见 [CHANGELOG.md](CHANGELOG.md)。

项目采用 [GPL-3.0-or-later](LICENSE)，第三方来源和许可证见 [THIRD_PARTY.md](THIRD_PARTY.md) 与 licenses 目录。

# MiniLyrics

Windows 原生桌面歌词小窗，基于 BetterNCM、RefinedNowPlayingNext 与 InfLink-rs。当前版本 **0.6.20**，作者 **shoebillback**。

显示端使用 C++ / Win32 / Direct2D / DirectWrite，JavaScript 负责歌词、播放信息及控制桥接。运行时无需安装 .NET、Node.js 或 WebView2。

## 功能

- 普通、逐字歌词与翻译；换句、点击跳转和前奏动画。
- 窗口置顶、鼠标穿透、拖动与缩放，多 DPI 屏幕适配。
- 滚轮浏览歌词，停止操作约四秒后回到播放位置。
- 封面、进度与播放控制，封面配色及 Lyricify 动态背景。

## 安装

需要 Windows 10/11 x64、网易云音乐 3.1.11+、BetterNCM ≥ 1.0.0、RefinedNowPlayingNext 与 InfLink-rs。

1. 从 GitHub Actions 的成功构建记录下载 `source-inclusive-plugin` 产物，解压得到 `MiniLyrics-0.6.20.plugin`；也可本地编译生成。商店收录后可直接从商店安装。
2. 完全退出网易云音乐与旧 MiniLyrics 小窗，将插件包放入 BetterNCM 数据目录下的 `plugins` 文件夹，替换旧包。
3. 重启网易云，在 MiniLyrics 设置中点击“打开 / 找回小窗”。

升级时应同时更新脚本和原生显示端，直接使用完整插件包即可。

## 项目结构

| 路径 | 用途 |
| --- | --- |
| `core.js` / `plugin.js` | 歌词数据处理与 BetterNCM 桥接 |
| `native/` | C++ 原生窗口、绘制、动画和背景 |
| `build.ps1` | 编译原生显示端并生成插件包 |
| `main.js` / `MiniLyricsHost.exe` | 随版本提供的运行文件 |
| `dist/` | 当前版本的插件安装包 |

## 构建

Windows 下安装 MinGW-w64 g++（需支持 Direct3D 11 / Direct2D）与 PowerShell，将 g++ 加入 PATH。

```powershell
powershell -NoProfile -File .\build.ps1 -OutputDirectory .\dist
```

默认输出到 `dist/`。插件包同时附带完整源码及许可证。

仅重新生成脚本及插件包，使用随仓库提供的原生程序：

```powershell
powershell -NoProfile -File .\build.ps1 -SkipNativeBuild -OutputDirectory .\dist
```

## 版本与许可证

0.6.20 将背景与歌词默认帧率调整为 60 FPS，默认窗口为最小尺寸 360×130（随系统缩放调整）。已保存的个人设置继续保留；“重置位置和尺寸”恢复最小窗口。详见 [CHANGELOG.md](CHANGELOG.md)。

项目采用 [GPL-3.0-or-later](LICENSE)，第三方来源和许可证见 [THIRD_PARTY.md](THIRD_PARTY.md) 与 licenses 目录。

## 插件商店发布

源码在 main 分支；plugin-release 分支由 GitHub Actions 手动发布任务生成，包含自动编译的原生程序和商店运行文件。不要手工复制本地 EXE 到发行分支。

在 Actions 中运行 Build MiniLyrics，选择 main 并勾选 publish。构建与脚本回归成功后才更新 plugin-release；普通 push/PR 只构建并上传检查产物。发布分支中 SOURCE.txt 指向确切源码提交和构建记录，SHA256SUMS.txt 记录运行文件摘要。

上架登记使用 shoebillback/MiniLyrics 的 plugin-release 分支。首次提交前需完成一次线上构建发布，并在实际网易云环境验证安装、依赖、逐字歌词和控制操作。版本升级同时更新 manifest.json 和 CHANGELOG.md，再手动发布。

许可证为 GPL-3.0-or-later，第三方声明保持不变；设置页提供源码和问题反馈入口。

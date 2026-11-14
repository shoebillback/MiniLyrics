# MiniLyrics

Windows 原生桌面歌词小窗，基于 BetterNCM 与 RefinedNowPlayingNext。当前版本 **0.7.8**，作者 **shoebillback**。

显示端使用 C++ / Win32 / Direct2D / DirectWrite，JavaScript 负责歌词、播放信息及控制桥接。运行时无需安装 .NET、Node.js 或 WebView2。

## 功能

- 普通、逐字歌词与翻译；换句、点击跳转和前奏动画。
- 窗口置顶、鼠标穿透、拖动与缩放，多 DPI 屏幕适配。
- 滚轮浏览歌词，停止操作约四秒后回到播放位置。
- 封面、进度与播放控制，封面配色及 Lyricify 动态背景。

## 安装

需要 Windows 10/11 x64、网易云音乐 3.1.11+、BetterNCM ≥ 1.0.0、RefinedNowPlayingNext。

1. 从 GitHub Actions 的成功构建记录下载 `source-inclusive-plugin` 产物，解压得到 `MiniLyrics-<版本号>.plugin`；也可本地编译生成。商店收录后可直接从商店安装。
2. 完全退出网易云音乐与旧 MiniLyrics 小窗，将插件包放入 BetterNCM 数据目录下的 `plugins` 文件夹，替换旧包。
3. 重启网易云，在 MiniLyrics 设置中点击“打开 / 找回小窗”。

升级时应同时更新脚本和原生显示端，直接使用完整插件包即可。

## 项目结构

| 路径 | 用途 |
| --- | --- |
| `core.js` / `player.js` / `plugin.js` | 歌词数据处理与 BetterNCM 桥接 |
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

0.7.0 使用独立网易云 3.x 播放适配层，已移除 InfLink 依赖。详见 [CHANGELOG.md](CHANGELOG.md)。

项目采用 [GPL-3.0-or-later](LICENSE)，第三方来源和许可证见 [THIRD_PARTY.md](THIRD_PARTY.md) 与 licenses 目录。

## 插件商店发布

源码在 main 分支；plugin-release 分支由 GitHub Actions 手动发布任务生成，包含自动编译的原生程序和商店运行文件。不要手工复制本地 EXE 到发行分支。

在 Actions 中运行 Build MiniLyrics，选择 main 并勾选 publish。构建与脚本回归成功后才更新 plugin-release；普通 push/PR 只构建并上传检查产物。发布分支中 SOURCE.txt 指向确切源码提交和构建记录，SHA256SUMS.txt 记录运行文件摘要。

上架登记使用 shoebillback/MiniLyrics 的 plugin-release 分支。首次提交前需完成一次线上构建发布，并在实际网易云环境验证安装、依赖、逐字歌词和控制操作。版本升级同时更新 manifest.json 和 CHANGELOG.md，再手动发布。

许可证为 GPL-3.0-or-later，第三方声明保持不变；设置页提供源码和问题反馈入口。

## 当前开发状态

0.7.0 移除 InfLink 安装与运行依赖，保留 RNP 歌词来源。player.js 独立适配网易云 3.x：读取播放数据，在已有播放器回调上订阅当前曲目的原生进度/状态/跳转事件，并通过播放器现有操作发送控制命令；不包装 channel.call，不修改其他插件。

冷启动时不为缺失的进度、状态或跳转事件创建占位；已有回调可追加订阅。跳转命令返回后立即再检查一次，异步初始化继续通过状态同步与时间轴采样补接。已有回调的归属未经确认，其他插件先占位时仍可能妨碍原版首次注册；不凭函数名称、源码或延时判断初始化，也不新增 Bridge 自动切换或强制重绑。无法检查回调表时等待接口就绪。仅移除自己的事件回调。切歌清空进度，实际 Seek 成功后才通知小窗；未知身份或旧曲目的事件不采纳。启动恢复只接受当前歌曲的存储位置，不能覆盖更新的进度。总时长及封面等元数据更新不触发切歌重置。接口未就绪时等待并重试，不使用 InfLink 后备路径。

兼容范围：网易云 3.x 当前内部模块结构；不声明支持 2.x。播放模块、DVA store 或可独立卸载的事件桥缺失时不会强行接管全局调用。卸载 InfLink 会同时移除它提供的 SMTC 等功能，MiniLyrics 不接管这些功能。

验证：适配层与小窗整合的隔离测试、原有歌词/跳转/配置回归通过；仍需在实际网易云中验证无 InfLink 启动、暂停/恢复、双向跳转、快速切歌、本地歌曲以及与 RNP/LyricBarNext 同时运行。未修改已安装插件，未发布商店。

0.7.7 的诊断位于 `window.__miniLyrics.getDiagnostics().playerDiagnostics`：区分控制模块就绪、当前订阅状态、实际进度收取/接受时间、过滤原因及断流。时间戳在真实播放器环境中采用页面单调时钟，`progressAgeMs` 为最后一次接受实际 PlayProgress 后经过的毫秒数；尚未收到时为 null。历史保存位置和成功 Seek 分别标记为 restore、seek，不计入持续实际进度，也不凭保存位置确认跳转。`originalInitialization` 保持 unverified，不能据订阅成功断言原版已初始化。

0.7.8 的播放按钮调用原版 playing/switchResumeOrPause，由原版状态决定播放或暂停。首次拖动进度条时，先写入原版恢复/加载位置并请求正常恢复播放；有 playId 且不处于恢复或首次加载时才直接跳转。加载中保留最近一次目标，暂停、切歌、关闭或卸载会取消 MiniLyrics 的后续补发。已建立会话后的暂停跳转保持暂停。初始化参考 InfLink V3 先解析 AudioPlayer，再接入 Bridge；不包装共享调用、不自动切换 Bridge。

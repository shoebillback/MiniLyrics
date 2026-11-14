# MiniLyrics

基于 BetterNCM 与 RefinedNowPlayingNext 的 Windows 原生桌面歌词小窗。当前版本 **0.7.9**，作者 **shoebillback**。

## 功能

- 普通歌词、逐字歌词与翻译，支持换句和前奏动画。
- 点击歌词或拖动进度条跳转，控制播放、暂停和切歌。
- 滚轮平滑浏览歌词，暂停时也可使用；约 4 秒无操作后回到当前句。
- 窗口置顶、鼠标穿透、拖动与缩放，多 DPI 屏幕适配。
- 专辑封面、封面配色与 Lyricify 动态背景。

## 安装

需要 Windows 10/11 x64、网易云音乐 3.1.11+、BetterNCM ≥ 1.0.0 和 RefinedNowPlayingNext。无需另装 .NET、Node.js 或 WebView2。

1. 从 [dist](dist/) 下载对应版本的 `.plugin` 安装包。
2. 完全退出网易云音乐和旧 MiniLyrics 小窗，将安装包放入 BetterNCM 数据目录的 `plugins` 文件夹，替换旧包。
3. 重启网易云，在 MiniLyrics 设置中点击“打开 / 找回小窗”。

升级时请替换完整安装包。开启鼠标穿透后，可在设置页点击“退出鼠标穿透”恢复操作。

## 构建

Windows 下安装 MinGW-w64 g++（支持 Direct3D 11 / Direct2D）和 PowerShell，将 g++ 加入 PATH，然后运行：

```powershell
powershell -NoProfile -File .\build.ps1
```

安装包输出至 `dist/`，附带完整源码及许可证。JavaScript 源码位于 `core.js`、`player.js`、`plugin.js`，原生窗口与绘制源码位于 `native/`。

`main` 保存源码；`plugin-release` 由 GitHub Actions 的 `Build MiniLyrics` 手动发布任务（勾选 `publish`）生成商店运行文件。

## 更新与反馈

版本变化见 [CHANGELOG.md](CHANGELOG.md)。遇到问题请在 [Issues](https://github.com/shoebillback/MiniLyrics/issues) 提供插件与网易云版本、复现步骤。

仅支持网易云音乐 3.x；播放同步依赖客户端内部接口，客户端更新可能影响兼容性。

## 许可证

项目采用 [GPL-3.0-or-later](LICENSE)。第三方来源与署名见 [THIRD_PARTY.md](THIRD_PARTY.md)，第三方许可证见 [licenses](licenses/)。

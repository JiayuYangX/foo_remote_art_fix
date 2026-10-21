# Remote Art Fix

A foobar2000 component that lets the core artwork manager return the **embedded**
artwork of remote (WebDAV / network) tracks — **only when foobar2000 itself can
read that artwork** (the cover is visible in the track's Properties dialog;
confirmed with WebDAV). It does **not** fetch artwork from any online source or
artwork service.

---

## Precondition

The cover must be readable by foobar2000 itself. The practical test: open the
track's Properties dialog. If the cover is shown there, this component can make
the artwork manager return it. If it is not shown (no embedded artwork, or the
format/extractor cannot read it), this component cannot provide one either.

## Scope and confirmed sources

* Confirmed working: WebDAV (native in foobar2000 2.x; tested with 2.25.8).
* The component adds neither format nor source support. It only works when
  foobar2000's own extractor can read the embedded artwork over that source —
  the practical test is the standard track Properties dialog.
* Local network shares (mapped drives, UNC paths) are treated as local by the
  core (`is_remote()` may return false for mounted shares); the core's normal
  artwork path already applies there, so this component is not needed.
* Other remote sources (UPnP/DLNA, HTTP(S) streams, cloud plugins, ...) are not
  covered by the core extractor and have not been tested.

## The problem

For remote files (`filesystem::is_remote() == true`) the core artwork pipelines
(`album_art_manager_v2`, `imageLoader`'s `trackimage://` protocol) deliberately
do not read embedded artwork. The guard exists to protect against cheap, frequent
UI queries (grids, lists, now-playing views) triggering downloads of large
amounts of remote media - a library of a few dozen albums could easily mean
hundreds of megabytes of traffic just to render thumbnails.

The side effect: on network sources such as WebDAV, themes and any component
using those pipelines show no covers at all - even though the artwork is embedded
in the files and the file properties dialog (which reads artwork through the
explicit extractor path) displays it fine.

That dialog is the reference for this component: it uses the same explicit
extractor path. If it shows no cover, the embedded artwork is not readable and
this component cannot provide one either.

Measured behavior for the same remote FLAC file:

| Call | Without this component | With this component |
|---|---|---|
| `album_art_manager_v2::open().query(front)` | `not found`, 0 ms | `243,998 bytes`, ~0.7 s |
| `imageLoader` `trackimage://...` | fails instantly | succeeds |
| `album_art_manager_v2::open().query(front)` on a local file | works as usual | unchanged |

The test file is a remote FLAC whose embedded artwork is readable by the core
(visible in Properties); the component does not add artwork to files that lack
it.

## What this component does

The SDK provides an `album_art_fallback` entry point: the last source the
artwork manager consults when every other source (embedded, external folder
images, other fallbacks) returned nothing. This component registers one:

1. the artwork manager asks it for the artwork of a remote track;
2. it opens the track through the regular format extractors
   (`album_art_extractor::g_open()`), i.e. the exact operation the core skips
   for remote files, and extracts the requested artwork type;
3. already extracted entries are cached in the returned instance, so the
   forthcoming `query()` does not hit the network a second time.

This is not a remote artwork downloader: it performs the same embedded-artwork
extraction the Properties dialog uses and exposes the result through the
`album_art_fallback` mechanism; it never contacts online artwork services.

Local files are explicitly ignored - the core handles them correctly, and this
component never changes their behavior.

## Install

Copy `foo_remote_art_fix.dll` into the foobar2000 `user-components` folder and
restart foobar2000:

* portable install: `<foobar2000 folder>\profile\user-components\`
* regular install: `%APPDATA%\foobar2000-v2\user-components\`

No configuration needed.

## Notes and caveats

* The first artwork request per album reads the embedded artwork from the
  remote file (about 0.7-1.5 s in our tests). UI components that request
  artwork synchronously on the main thread will block for that long - prefer
  asynchronous artwork requests (the norm).
* Only embedded artwork is handled; folder images (`cover.jpg`, etc.) remain
  the core's responsibility. Which formats are supported is decided by
  foobar2000 itself (e.g. FLAC, MP3, MP4/M4A, Ogg Vorbis/Opus, WavPack); the
  authoritative test remains the Properties dialog.
* Not an online artwork fetcher: it only serves embedded artwork that
  foobar2000's own extractor can read. Remote tracks whose Properties dialog
  shows no cover cannot be helped by this component.
* This component is a workaround for a deliberate core behavior, not an
  official fix.

## Building

Requirements: Visual Studio with the "Desktop development with C++" workload
(MSVC v143 toolset + Windows SDK). The foobar2000 SDK (2026-10-01 release)
is bundled under `sdk/` (redistributed under its own license,
`sdk/sdk-license.txt`), so no separate download is needed.

In Visual Studio: open `foo_remote_art_fix.vcxproj`, pick `Release` and the
target platform (`Win32` or `x64`), then Build. The DLL lands in
`Win32\Release\` or `x64\Release\`.

From a command line (Developer Command Prompt, or any shell that can reach
MSBuild):

```
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=Win32
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=x64
```

The project references the SDK's `foobar2000_SDK`, `pfc` and
`foobar2000_component_client` projects; the SDK location comes from the
`SdkDir` / `SdkRoot` properties near the top of the `.vcxproj`, which default
to the bundled `sdk\` folder (override with `-p:SdkDir=...` / `-p:SdkRoot=...`
to build against a different copy).

Alternatively run `.\build.ps1` in PowerShell: it builds both architectures
and produces `dist\foo_remote_art_fix-<version>.fb2k-component` (x86 at the
archive root, x64 under `x64/`, i.e. the standard component package layout;
the version is read from the source). The script locates the SDK in this
order: `-SdkRoot <path>` argument, `FOOBAR2000_SDK_ROOT` environment
variable, then the bundled `sdk\` folder. `-SkipBuild` packages existing
binaries without requiring the SDK.

---

> # 中文

一个让核心封面管理器能正确返回远程（WebDAV / 网络）曲目**内嵌封面**的 foobar2000 组件——**前提是 foobar2000 自身能读取该封面**（曲目“属性”面板里能看见封面；WebDAV 已确认）。它不会从网络或任何在线封面源抓取封面。

## 前提

封面必须能被 foobar2000 自身读取。判断方法：打开曲目的“属性”面板——那里能看到封面，本组件就能让封面管理器正确返回；那里看不到（没有内嵌封面，或格式/读取器不支持），本组件也无法提供。

## 适用范围与已确认来源

* 已确认可用：WebDAV（foobar2000 2.x 原生支持；在 2.25.8 上实测）。
* 组件本身不增加任何格式或来源支持，只适用于“foobar2000 自带提取器能通过该来源读到内嵌封面”的情况——判断方法就是标准的曲目“属性”面板。
* 局域网共享（映射盘符、UNC 路径）被核心当作本地文件处理（挂载共享的 `is_remote()` 可能返回 false），核心的正常封面路径本就适用，不需要本组件。
* 其他网络来源（UPnP/DLNA、HTTP(S) 流、网盘插件等）不在核心提取器覆盖范围内，尚未测试。

## 问题背景

对于远程文件（`filesystem::is_remote() == true`），核心封面管线
（`album_art_manager_v2`、imageLoader 的 `trackimage://`）**故意不读取内嵌封面**。
这个限制是为了避免网格、列表、正在播放这类高频 UI 查询去下载大量远程数据——
几十张专辑的媒体库，光渲染缩略图就可能产生几百 MB 流量。

副作用是：在 WebDAV 等网络音源上，主题和其它依赖该管线的组件都显示不出封面，
即使封面就嵌在文件里、文件属性对话框（走显式提取路径）能正常显示。

这里的“属性”面板就是本组件的判据：它走的是同一条显式提取路径。如果属性面板里看不到封面，说明内嵌封面不可读，本组件也无能为力。

同一个远程 FLAC 文件的实测对比：

| 调用 | 没有本组件 | 装本组件后 |
|---|---|---|
| `album_art_manager_v2::open().query(front)` | 0ms 返回 `not found` | ~0.7s 返回 `243,998 字节` |
| `imageLoader` `trackimage://...` | 立即失败 | 成功 |
| 同上调用（本地文件） | 正常 | 不受影响 |

测试文件：带内嵌封面且核心可读取（属性面板可见）的远程 FLAC；组件不会为本身没有封面的文件凭空生成封面。

## 组件原理

SDK 提供了 `album_art_fallback` 入口：当封面管理器把所有来源（内嵌、外部图片、
其他 fallback）都问完仍一无所获时，才会询问的最后一环。本组件注册了这样一个实现：

1. 管理器来问某远程曲目的封面；
2. 它用常规格式提取器（`album_art_extractor::g_open()`）打开该文件——也就是
   核心对远程文件跳过的那一步操作——并提取请求的封面类型；
3. 已提取的条目会缓存进返回的实例，随后真正的 `query()` 不会二次下载。

本组件不是远程封面下载器：它执行的是与“属性”面板相同的内嵌封面提取，并通过 `album_art_fallback` 把结果交给封面管理器；不访问任何在线封面服务。

本地文件被显式排除：核心对本地文件处理得很好，本组件不改变其任何行为。

## 安装

把 `foo_remote_art_fix.dll` 复制到 foobar2000 的 `user-components` 目录后重启：

* 便携版：`<foobar2000 目录>\profile\user-components\`
* 安装版：`%APPDATA%\foobar2000-v2\user-components\`

无需任何配置。

## 注意事项

* 每个专辑第一次请求封面会从远程文件读取内嵌封面（测试中约 0.7–1.5 秒）；
  如果某个 UI 组件在主线程同步请求封面，会阻塞同样长的时间——建议使用异步请求。
* 只处理内嵌封面；同目录图片（`cover.jpg` 等）仍由核心自身的外部来源负责。支持哪些格式由 foobar2000 自身决定（如 FLAC、MP3、MP4/M4A、Ogg Vorbis/Opus、WavPack 等），最终仍以属性面板为准。
* 不是在线封面获取器：只对 foobar2000 自身能读取的内嵌封面生效（以属性面板可见为准）；属性面板看不到封面的远程曲目，本组件无法提供。
* 本组件是对核心既定行为的绕行方案，并非官方修复。

## 构建

需要：Visual Studio（安装时勾选“使用 C++ 的桌面开发”，含 MSVC v143 与
Windows SDK）。foobar2000 SDK（2026-10-01 版）已随仓库放在 `sdk\` 下
（按自身许可分发，见 `sdk\sdk-license.txt`），无需单独下载。

在 Visual Studio 里：打开 `foo_remote_art_fix.vcxproj`，选择 `Release` 与目标
平台（`Win32` 或 `x64`），然后“生成”。产物分别在 `Win32\Release\` 和
`x64\Release\`。

命令行（VS 开发人员命令提示符，或任何能找到 MSBuild 的 shell）：

```
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=Win32
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=x64
```

工程通过 SDK 自带的 `foobar2000_SDK`、`pfc`、`foobar2000_component_client`
三个子工程一起编译；SDK 路径来自 vcxproj 顶部的 `SdkDir` / `SdkRoot` 属性，
默认指向仓库自带的 `sdk\` 目录（可用 `-p:SdkDir=...`、`-p:SdkRoot=...`
指向其他 SDK 副本）。

也可以在 PowerShell 里运行 `.\build.ps1`：自动构建两个架构并生成
`dist\foo_remote_art_fix-<版本>.fb2k-component`（32 位在包根目录、64 位在
`x64/` 子目录，即标准组件包布局；版本号从源码读取）。SDK 查找顺序：
`-SdkRoot <路径>` 参数 > 环境变量 `FOOBAR2000_SDK_ROOT` > 仓库自带的
`sdk\`；`-SkipBuild` 跳过编译直接用现有产物打包（不需要 SDK）。

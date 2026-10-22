# Remote Art Fix

A foobar2000 component that lets the core artwork manager return the **embedded** artwork of remote tracks. For filesystem-backed sources (e.g. WebDAV) it works when foobar2000 itself can read the artwork (the cover is visible in the track's Properties dialog). For plain HTTP(S) direct links it works when the server supports Range requests. It does **not** download artwork from online services or databases — only the artwork embedded in the media file itself.

---

## Precondition

* **Filesystem-backed sources (WebDAV, etc.)**: the cover must be readable by foobar2000 itself. The practical test: open the track's Properties dialog. If the cover is shown there, this component can make the artwork manager return it.
* **Plain HTTP(S) direct links**: the URL must point to a complete media file with embedded artwork, and the server must support HTTP Range requests (most CDNs / object storage do). The Properties dialog test does not apply to plain URLs.
* In both cases, if the format has no embedded artwork or the core extractor cannot read it, this component cannot provide one either.

## Scope and confirmed sources

* WebDAV: confirmed (native in foobar2000 2.x; tested with 2.25.10).
* Plain HTTP(S) direct links: supported since 1.1.0 when the server supports Range requests; servers without Range support are not handled.
* Any other source that exposes a seekable filesystem is handled automatically by the filesystem path (no additional code needed).
* Local network shares (mapped drives, UNC paths) are treated as local by the core (`is_remote()` may return false for mounted shares); the core's normal artwork path already applies there, so this component is not needed.
* Sources that are not exposed as seekable files (UPnP/DLNA, plugin-specific streams, live radio) are not supported.

## The problem

For remote files (`filesystem::is_remote() == true`) the core artwork pipelines (`album_art_manager_v2`, `imageLoader`'s `trackimage://` protocol) deliberately do not read embedded artwork. The guard exists to protect against cheap, frequent UI queries (grids, lists, now-playing views) triggering downloads of large amounts of remote media - a library of a few dozen albums could easily mean hundreds of megabytes of traffic just to render thumbnails.

The side effect: on network sources such as WebDAV, themes and any component using those pipelines show no covers at all - even though the artwork is embedded in the files and the file properties dialog (which reads artwork through the explicit extractor path) displays it fine.

For filesystem-backed sources, that dialog is the reference: it uses the same explicit extractor path. If it shows no cover, the embedded artwork is not readable and this component cannot provide one either.

Measured behavior for the same remote FLAC file:

| Call | Without this component | With this component |
|---|---|---|
| `album_art_manager_v2::open().query(front)` | `not found`, 0 ms | `243,998 bytes`, ~0.7 s |
| `imageLoader` `trackimage://...` | fails instantly | succeeds |
| `album_art_manager_v2::open().query(front)` on a local file | works as usual | unchanged |

The test file is a remote FLAC whose embedded artwork is readable by the core (visible in Properties); the component does not add artwork to files that lack it.

## What this component does

The SDK provides an `album_art_fallback` entry point: the last source the artwork manager consults when every other source (embedded, external folder images, other fallbacks) returned nothing. This component registers one. It does not add a new artwork source: it re-opens the file itself through foobar2000's own extractors and exposes the result to the artwork manager.

### Fallback order

The component runs only after the artwork manager has exhausted its normal sources. It then works in two stages:

1. **Standard extraction.** It first calls the regular album art extractor for the path (`album_art_extractor::g_open()`), the same extraction path the Properties dialog uses. This respects the normal input selection and any enabled tag/art filters.
2. **Direct input fallback.** If the standard extraction returns none of the requested artwork types, the component enumerates the enabled inputs in the user's decoder priority, selects those that claim the path, and calls their album art interface directly. This bypasses input wrappers and tag/art filters installed by other components. The first input that actually returns artwork for a requested type wins.

Artwork types obtained by the fallback are cached in the returned instance; later queries for those types are served from the cache. Other artwork types are requested from the chosen input. The same two stages run for plain HTTP(S) direct links, using the `http_client` file; the file is rewound to the beginning before each input attempt and must support Range requests.

The direct fallback is generic: it does not target any specific component. It exists because some components wrap inputs or filter tag/art reads; asking the enabled inputs directly avoids that interception.

### Conditions

| Condition | Behavior |
|---|---|
| Local file | Not handled - the core already reads it normally. |
| Remote or unrecognized path | Handled. |
| Standard extraction returns artwork | Used; the direct fallback is not attempted. |
| Standard extraction returns no artwork | Direct input fallback runs. |
| No enabled input returns artwork | Reported as "not found". |
| File has no embedded artwork | Reported as "not found". |
| HTTP(S) without Range support | Not supported. |

### Transports

- **Filesystem-backed sources** (WebDAV, etc.): the track is opened through the regular format extractors, i.e. the operation the core skips for remote files.
- **Plain HTTP(S) direct links**: the link is opened with the SDK's HTTP client (Range requests provide random access); the container format is determined from the URL extension / Content-Type / magic bytes, and the open file is passed to the format extractor via `p_filehint`.
- The extractor instance is opened only once per query, and extracted entries are cached in the returned instance.

This is not a remote artwork downloader: it performs the same embedded-artwork extraction the Properties dialog uses and exposes the result through the `album_art_fallback` mechanism; it never contacts online artwork services.

Local files are explicitly ignored - the core handles them correctly, and this component never changes their behavior.

## Install

Copy `foo_remote_art_fix.dll` into the foobar2000 `user-components` folder and restart foobar2000:

* portable install: `<foobar2000 folder>\profile\user-components\`
* regular install: `%APPDATA%\foobar2000-v2\user-components\`

No configuration needed.

## Notes and caveats

* The first artwork request per album reads the embedded artwork from the remote file (about 0.7-1.5 s in our tests). Read operations may issue multiple HTTP Range requests (say, one for the header, one for the tail). UI components that request artwork synchronously on the main thread will block for that long - prefer asynchronous artwork requests (the norm).
* Only embedded artwork is handled; folder images (`cover.jpg`, etc.) remain the core's responsibility. Which formats are supported is decided by foobar2000 itself (e.g. FLAC, MP3, MP4/M4A, Ogg Vorbis/Opus, WavPack); the authoritative test remains the Properties dialog.
* Plain HTTP(S) direct links require server-side Range support. Live streams, servers without Range support, and links requiring authentication headers are not supported.
* Not an online artwork fetcher: it only serves the artwork embedded in the media file (readable by foobar2000's own extractor). It does not query any artwork database, and cannot produce art for files that do not contain it.
* Third-party input wrappers/filters (e.g. foo_external_tags) can suppress artwork on the standard path; the direct fallback above reads the artwork through the enabled inputs. If the Properties dialog also shows no cover, that component is filtering the path too; disabling its art handling (foo_external_tags: "Enable art support in external tags") restores the core behavior.
* This component is a workaround for a deliberate core behavior, not an official fix.

## Building

Requirements: Visual Studio with the "Desktop development with C++" workload (MSVC v143 toolset + Windows SDK). The foobar2000 SDK (2026-10-01 release) is bundled under `sdk/` (redistributed under its own license, `sdk/sdk-license.txt`), so no separate download is needed.

In Visual Studio: open `foo_remote_art_fix.vcxproj`, pick `Release` and the target platform (`Win32` or `x64`), then Build. The DLL lands in `Win32\Release\` or `x64\Release\`.

From a command line (Developer Command Prompt, or any shell that can reach MSBuild):

```
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=Win32
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=x64
```

The project references the SDK's `foobar2000_SDK`, `pfc` and `foobar2000_component_client` projects; the SDK location comes from the `SdkDir` / `SdkRoot` properties near the top of the `.vcxproj`, which default to the bundled `sdk\` folder (override with `-p:SdkDir=...` / `-p:SdkRoot=...` to build against a different copy).

Alternatively run `.\build.ps1` in PowerShell: it builds both architectures and produces `dist\foo_remote_art_fix-<version>.fb2k-component` (x86 at the archive root, x64 under `x64/`, i.e. the standard component package layout; the version is read from the source). The script locates the SDK in this order: `-SdkRoot <path>` argument, `FOOBAR2000_SDK_ROOT` environment variable, then the bundled `sdk\` folder. `-SkipBuild` packages existing binaries without requiring the SDK.

---

> # 中文

一个让核心封面管理器能正确返回远程曲目**内嵌封面**的 foobar2000 组件。对文件系统型来源（如 WebDAV），前提是 foobar2000 自身能读取该封面（曲目“属性”面板里能看见封面）；对普通 HTTP(S) 直链，前提是服务器支持 Range 请求。它不会从在线封面服务或数据库下载封面，只读取媒体文件本身的内嵌图。

## 前提

* **文件系统型来源（WebDAV 等）**：封面必须能被 foobar2000 自身读取。判断方法：打开曲目的“属性”面板——那里能看到封面，本组件就能让封面管理器正确返回。
* **普通 HTTP(S) 直链**：URL 必须指向完整的媒体文件（含内嵌封面），且服务器支持 HTTP Range 请求（多数 CDN/对象存储都支持）；“属性”面板测试不适用于裸 URL。
* 两种情况都一样：文件没有内嵌封面、或核心提取器读不了该格式时，本组件无法提供封面。

## 适用范围与已确认来源

* WebDAV：已确认（foobar2000 2.x 原生支持；在 2.25.10 上实测）。
* 普通 HTTP(S) 直链：1.1.0 起支持；要求服务器支持 Range 请求，不支持 Range 的服务器无法处理。
* 其他以“可 seek 文件系统”形式出现的来源会自动走文件系统通道，无需额外代码。
* 局域网共享（映射盘符、UNC 路径）被核心当作本地文件处理（挂载共享的 `is_remote()` 可能返回 false），核心的正常封面路径本就适用，不需要本组件。
* 不是可 seek 文件的来源（UPnP/DLNA、插件专用的流、网络电台直播）不支持。

## 问题背景

对于远程文件（`filesystem::is_remote() == true`），核心封面管线（`album_art_manager_v2`、imageLoader 的 `trackimage://`）**故意不读取内嵌封面**。这个限制是为了避免网格、列表、正在播放这类高频 UI 查询去下载大量远程数据——几十张专辑的媒体库，光渲染缩略图就可能产生几百 MB 流量。

副作用是：在 WebDAV 等网络音源上，主题和其它依赖该管线的组件都显示不出封面，即使封面就嵌在文件里、文件属性对话框（走显式提取路径）能正常显示。

对文件系统型来源，这里的“属性”面板就是本组件的判据：它走的是同一条显式提取路径。如果属性面板里看不到封面，说明内嵌封面不可读，本组件也无能为力。

同一个远程 FLAC 文件的实测对比：

| 调用 | 没有本组件 | 装本组件后 |
|---|---|---|
| `album_art_manager_v2::open().query(front)` | 0ms 返回 `not found` | ~0.7s 返回 `243,998 字节` |
| `imageLoader` `trackimage://...` | 立即失败 | 成功 |
| 同上调用（本地文件） | 正常 | 不受影响 |

测试文件：带内嵌封面且核心可读取（属性面板可见）的远程 FLAC；组件不会为本身没有封面的文件凭空生成封面。

## 组件原理

SDK 提供了 `album_art_fallback` 入口：当封面管理器把所有来源（内嵌、外部图片、其他 fallback）都问完仍一无所获时，才会询问的最后一环。本组件注册了这样一个实现；它不新增图源，只是重新打开曲目文件、走 foobar2000 自带的提取器，再把结果交给封面管理器。

### 回退顺序

只有封面管理器用尽常规来源后，本组件才会运行，分两步：

1. **标准提取**：先对该路径调用常规封面提取器（`album_art_extractor::g_open()`），也就是“属性”面板使用的同一条提取路径。这一步遵循正常的输入选择以及已启用的标签/封面过滤器。
2. **直接输入回退**：如果标准提取没有返回请求的任何封面类型，组件会按用户的解码器优先级枚举已启用的输入，筛出认领该路径的输入，直接调用它们的封面接口。这会绕过其它组件安装的输入包装和标签/封面过滤器；第一个真正返回请求封面的输入胜出。

回退得到的封面类型会缓存在返回的实例中，后续同一类型的查询直接命中；其它类型向选中的输入请求。普通 HTTP(S) 直链同样走这两步，使用 `http_client` 打开的文件；每次尝试输入前都会把文件指针回到开头，且要求服务器支持 Range 请求。

直接回退是通用的，不针对任何特定组件。它存在的原因是有组件会包装输入或过滤标签/封面读取；直接询问已启用的输入可以绕开这层拦截。

### 条件

| 条件 | 行为 |
|---|---|
| 本地文件 | 不处理——核心本来就能正常读取。 |
| 远程或未识别路径 | 处理。 |
| 标准提取返回封面 | 直接使用，不触发直接回退。 |
| 标准提取没有返回封面 | 执行直接输入回退。 |
| 所有已启用输入都没有封面 | 返回“未找到”。 |
| 文件本身没有内嵌封面 | 返回“未找到”。 |
| HTTP(S) 不支持 Range | 不支持。 |

### 传输方式

- **文件系统型来源**（WebDAV 等）：通过常规格式提取器打开曲目——也就是核心对远程文件跳过的那一步操作。
- **普通 HTTP(S) 直链**：先用 SDK 的 HTTP 客户端打开链接（靠 Range 请求获得随机访问），再根据 URL 扩展名 / Content-Type / 文件头魔数判断容器格式，通过 `p_filehint` 把已打开的文件交给格式提取器。
- 每个实例只打开一次提取器，已提取的条目会缓存。

本组件不是远程封面下载器：它执行的是与“属性”面板相同的内嵌封面提取，并通过 `album_art_fallback` 把结果交给封面管理器；不访问任何在线封面服务或数据库。

本地文件被显式排除：核心对本地文件处理得很好，本组件不改变其任何行为。

## 安装

把 `foo_remote_art_fix.dll` 复制到 foobar2000 的 `user-components` 目录后重启：

* 便携版：`<foobar2000 目录>\profile\user-components\`
* 安装版：`%APPDATA%\foobar2000-v2\user-components\`

无需任何配置。

## 注意事项

* 每个专辑第一次请求封面会从远程文件读取内嵌封面（测试中约 0.7–1.5 秒）；读取过程可能发出多次 HTTP Range 请求（例如头部一次、尾部一次）。如果某个 UI 组件在主线程同步请求封面，会阻塞同样长的时间——建议使用异步请求。
* 只处理内嵌封面；同目录图片（`cover.jpg` 等）仍由核心自身的外部来源负责。支持哪些格式由 foobar2000 自身决定（如 FLAC、MP3、MP4/M4A、Ogg Vorbis/Opus、WavPack 等），最终仍以属性面板为准。
* 普通 HTTP(S) 直链要求服务器支持 Range 请求；直播流、不支持 Range 的服务器、需要认证请求头的链接不支持。
* 不是在线封面获取器：只提供媒体文件自带、且 foobar2000 提取器能读取的内嵌封面；不查询任何封面数据库，也无法为本身没有封面的文件生成封面。
* 与第三方输入包装/过滤器（如 foo_external_tags）共存：它们可能拦截标准路径的封面；上面的直接回退会通过已启用输入读取封面。如果“属性”面板也看不到封面，说明该组件同样拦截了那条路径；关掉它的封面支持（foo_external_tags：“Enable art support in external tags”）即可恢复核心行为。
* 本组件是对核心既定行为的绕行方案，并非官方修复。

## 构建

需要：Visual Studio（安装时勾选“使用 C++ 的桌面开发”，含 MSVC v143 与 Windows SDK）。foobar2000 SDK（2026-10-01 版）已随仓库放在 `sdk\` 下（按自身许可分发，见 `sdk\sdk-license.txt`），无需单独下载。

在 Visual Studio 里：打开 `foo_remote_art_fix.vcxproj`，选择 `Release` 与目标平台（`Win32` 或 `x64`），然后“生成”。产物分别在 `Win32\Release\` 和 `x64\Release\`。

命令行（VS 开发人员命令提示符，或任何能找到 MSBuild 的 shell）：

```
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=Win32
MSBuild foo_remote_art_fix.vcxproj -p:Configuration=Release -p:Platform=x64
```

工程通过 SDK 自带的 `foobar2000_SDK`、`pfc`、`foobar2000_component_client` 三个子工程一起编译；SDK 路径来自 vcxproj 顶部的 `SdkDir` / `SdkRoot` 属性，默认指向仓库自带的 `sdk\` 目录（可用 `-p:SdkDir=...`、`-p:SdkRoot=...` 指向其他 SDK 副本）。

也可以在 PowerShell 里运行 `.\build.ps1`：自动构建两个架构并生成 `dist\foo_remote_art_fix-<版本>.fb2k-component`（32 位在包根目录、64 位在 `x64/` 子目录，即标准组件包布局；版本号从源码读取）。SDK 查找顺序：`-SdkRoot <路径>` 参数 > 环境变量 `FOOBAR2000_SDK_ROOT` > 仓库自带的 `sdk\`；`-SkipBuild` 跳过编译直接用现有产物打包（不需要 SDK）。

# build.ps1 - 构建 foo_remote_art_fix 并打包为 .fb2k-component
#
# 用法：
#   .\build.ps1                  # 构建 x86 + x64（Release）并打包
#   .\build.ps1 -SkipBuild       # 跳过编译，用现有产物重新打包
#   .\build.ps1 -Clean           # 清理所有构建产物（含 sdk\ 内的编译产物）
#   .\build.ps1 -SdkRoot <路径>  # 使用指定位置的 SDK（默认：仓库自带的 sdk\）
#
# 若提示脚本被禁止运行：
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#
# 产物（版本号取自源码 DECLARE_COMPONENT_VERSION）：
#   dist\foo_remote_art_fix-<版本>.fb2k-component
#     ├── foo_remote_art_fix.dll        （32 位）
#     └── x64\foo_remote_art_fix.dll    （64 位）

[CmdletBinding()]
param(
    [string]$SdkRoot,
    [switch]$SkipBuild,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$Root    = $PSScriptRoot
$PkgName = 'foo_remote_art_fix'
$Project = Join-Path $Root "$PkgName.vcxproj"
$Source  = Join-Path $Root "$PkgName.cpp"
$Staging = Join-Path $Root 'staging'
$Dist    = Join-Path $Root 'dist'

# ---------- 清理 ----------
if ($Clean) {
    foreach ($item in @('Win32', 'x64', 'staging', 'dist', 'build')) {
        Remove-Item (Join-Path $Root $item) -Recurse -Force -ErrorAction SilentlyContinue
    }
    $bundledSdk = Join-Path $Root 'sdk'
    if (Test-Path $bundledSdk) {
        Get-ChildItem $bundledSdk -Recurse -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -in @('Release', 'Debug', 'x64', 'Win32') } |
            ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
        Get-ChildItem $bundledSdk -Recurse -File -Include '*.pch', '*.obj', '*.tlog', '*.iobj', '*.ipdb', '*.log' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    Write-Host '[OK] 已清理构建产物。'
    return
}

# ---------- 解析 SDK 路径：-SdkRoot > 环境变量 FOOBAR2000_SDK_ROOT > 仓库内 sdk\ ----------
if (-not $SdkRoot) { $SdkRoot = $env:FOOBAR2000_SDK_ROOT }
if (-not $SdkRoot) {
    $candidate = Join-Path $Root 'sdk'
    if (Test-Path (Join-Path $candidate 'foobar2000')) { $SdkRoot = $candidate }
}

# ---------- 编译（Release / Win32 + x64） ----------
if (-not $SkipBuild) {
    if (-not $SdkRoot -or -not (Test-Path (Join-Path $SdkRoot 'foobar2000'))) {
        throw '找不到 foobar2000 SDK（仓库自带的 sdk\ 应随仓库存在，也可用 -SdkRoot 或 FOOBAR2000_SDK_ROOT 指定）'
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = $null
    if (Test-Path $vswhere) {
        $msbuild = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild `
                             -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
    }
    if (-not $msbuild) {
        $msbuild = (Get-Item "${env:ProgramFiles}\Microsoft Visual Studio\*\*\MSBuild\Current\Bin\MSBuild.exe" -ErrorAction SilentlyContinue |
                    Select-Object -First 1).FullName
    }
    if (-not $msbuild) { throw '找不到 MSBuild，请安装 Visual Studio（含 C++ 生成工具）' }

    Write-Host "使用 SDK:     $SdkRoot"
    Write-Host "使用 MSBuild: $msbuild"
    foreach ($platform in @('Win32', 'x64')) {
        Write-Host ">>> 构建 $platform ..."
        & $msbuild $Project -nologo -v:minimal -m "-p:Configuration=Release" "-p:Platform=$platform" `
            "-p:SdkDir=$(Join-Path $SdkRoot 'foobar2000')" "-p:SdkRoot=$SdkRoot"
        if ($LASTEXITCODE -ne 0) { throw "$platform 构建失败" }
    }
}

# ---------- 打包：32 位在根目录、64 位在 x64/ 子目录（foobar2000 官方包布局） ----------
$x86Dll = Join-Path $Root "Win32\Release\$PkgName.dll"
$x64Dll = Join-Path $Root "x64\Release\$PkgName.dll"
foreach ($dll in @($x86Dll, $x64Dll)) {
    if (-not (Test-Path $dll)) { throw "找不到 $dll，请先构建（去掉 -SkipBuild）" }
}

# 版本号取自源码里的 DECLARE_COMPONENT_VERSION（单一来源）
$match = [regex]::Match((Get-Content $Source -Raw -Encoding UTF8),
    'DECLARE_COMPONENT_VERSION\s*\(\s*"[^"]*"\s*,\s*"([0-9]+\.[0-9]+\.[0-9]+)"')
$pkgFile = if ($match.Success) { "$PkgName-$($match.Groups[1].Value).fb2k-component" } else { "$PkgName.fb2k-component" }

Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path (Join-Path $Staging 'x64') -Force | Out-Null
Copy-Item $x86Dll (Join-Path $Staging "$PkgName.dll")
Copy-Item $x64Dll (Join-Path $Staging "x64\$PkgName.dll")

# .fb2k-component 就是改名 zip；必须手动建条目用正斜杠——CreateFromDirectory 会把
# 路径写成反斜杠，foobar2000 将认不出 x64/ 子目录
Remove-Item $Dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Dist -Force | Out-Null
$pkg = Join-Path $Dist $pkgFile

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($pkg, [IO.Compression.ZipArchiveMode]::Create)
try {
    [void]$zip.CreateEntry('x64/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $Staging "$PkgName.dll"),     "$PkgName.dll")
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $Staging "x64\$PkgName.dll"), "x64/$PkgName.dll")
} finally {
    $zip.Dispose()
}

# 自检（必须在关闭后重新打开：Create 模式下 .Entries 枚举不可靠）
$zipCheck = [IO.Compression.ZipFile]::OpenRead($pkg)
try {
    foreach ($e in $zipCheck.Entries) {
        if ($e.FullName.Contains('\')) { throw "包内路径含反斜杠：$($e.FullName)" }
    }
    foreach ($name in @("$PkgName.dll", "x64/$PkgName.dll")) {
        if (-not ($zipCheck.Entries | Where-Object { $_.FullName -eq $name })) { throw "包内缺少：$name" }
    }
} finally {
    $zipCheck.Dispose()
}

Remove-Item $Staging -Recurse -Force -ErrorAction SilentlyContinue

Write-Host '[OK] 完成：'
Write-Host ("     {0}  ({1:N0} 字节)" -f $x86Dll, (Get-Item $x86Dll).Length)
Write-Host ("     {0}  ({1:N0} 字节)" -f $x64Dll, (Get-Item $x64Dll).Length)
Write-Host ("     {0}  ({1:N0} 字节)" -f $pkg,    (Get-Item $pkg).Length)
Write-Host '安装：双击 .fb2k-component 导入；或 foobar2000 > Preferences > Components > Install'

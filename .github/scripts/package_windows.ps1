param(
    [Parameter(Mandatory=$true)][string]$Art
)
# 将 sukic 工具链打包为「独立、可重定位」的 Windows 压缩包（开箱即用）。
# 用法：package_windows.ps1 <artifact-name>
# 产物：dist/<artifact-name>.zip
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Pkg  = Join-Path $Root "dist\$Art"

Remove-Item -Recurse -Force $Pkg -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$Pkg\bin", "$Pkg\lib\suki\runtime", "$Pkg\share\suki" | Out-Null

# 1) 编译器二进制（VS 多配置生成器：build\bin\Release\sukic.exe）
$Exe = (Get-ChildItem -Path $Root\build -Filter sukic.exe -Recurse | Select-Object -First 1).FullName
Copy-Item $Exe "$Pkg\bin\sukic.exe"

# 2) 预编译运行时对象（Windows 默认为 runtime.obj）
$Rt = (Get-ChildItem -Path $Root\build -Filter runtime.obj -Recurse | Select-Object -First 1).FullName
Copy-Item $Rt "$Pkg\lib\suki\runtime.obj"

# 3) 运行时源码（交叉编译目标时由 clang 即时编译 runtime.c）
Copy-Item $Root\src\runtime\runtime.c, $Root\src\runtime\runtime.h "$Pkg\lib\suki\runtime\"

# 4) 标准库（保留目录结构）
Copy-Item -Recurse $Root\src\stdlib "$Pkg\share\suki\stdlib"

# 5) 启动脚本（sukic 已支持环境变量重定位；LLVM 在 Windows 为静态链接，无需额外 dll）
@"
@echo off
setlocal
set "SCRIPT_DIR=%~dp0.."
set "SUKICODE_STDLIB_DIR=%SCRIPT_DIR%share\suki\stdlib"
set "SUKICODE_RUNTIME_OBJECT=%SCRIPT_DIR%lib\suki\runtime.obj"
set "SUKICODE_RUNTIME_SOURCE=%SCRIPT_DIR%lib\suki\runtime\runtime.c"
set "SUKICODE_RUNTIME_INCLUDE=%SCRIPT_DIR%lib\suki\runtime"
set "SUKICODE_CLANG_DRIVER=clang"
"%SCRIPT_DIR%bin\sukic.exe" %*
"@ | Set-Content -NoNewline -Encoding utf8NoBOM "$Pkg\bin\suki.bat"

# 6) 使用说明
@"
# SukiCode 独立工具链（Windows x86_64）
解压后运行 bin\suki.bat（或 bin\sukic.exe）即可。需要 clang 在 PATH 中（建议安装 LLVM）。
"@ | Set-Content -Encoding utf8NoBOM "$Pkg\README.md"

# 7) 打包 zip。使用 .NET 的 ZipFile.CreateFromDirectory 而非 Compress-Archive：
#    后者对「是否包含顶层目录」的处理存在歧义，这里显式要求包含基准目录，
#    保证压缩包顶层为 <artifact-name>/，与 Linux 包结构完全一致。
$Zip = Join-Path $Root "dist\$Art.zip"
Remove-Item -Force $Zip -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory(
    $Pkg, $Zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Host "打包产物: $Zip (大小: $((Get-Item $Zip).Length) bytes)"

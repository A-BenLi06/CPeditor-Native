#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Debug',
    [ValidateSet('x64', 'arm64')]
    [string] $Platform = 'x64',
    [string] $MSBuildPath,
    [string] $WindowsSdkVersion,
    [switch] $Launch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) {
    throw '此脚本仅支持 Windows 主机；不能在 macOS 上执行 Windows 构建或验收。'
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$windowsRoot = Join-Path $repositoryRoot 'apps/windows'
$projectPath = Join-Path $windowsRoot 'CPEditorNativeWindows/CPEditorNativeWindows.vcxproj'
$nugetConfigPath = Join-Path $windowsRoot 'NuGet.Config'
$restorePath = Join-Path $windowsRoot 'packages'
$outputRoot = Join-Path $repositoryRoot "out/windows/$Platform/$Configuration"
$layoutPath = Join-Path $outputRoot 'layout'

if ([string]::IsNullOrWhiteSpace($MSBuildPath)) {
    $installerRoot = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer'
    $vswherePath = Join-Path $installerRoot 'vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswherePath -PathType Leaf)) {
        throw '未找到 vswhere.exe；请安装 Visual Studio 2022 C++/WinUI 工具，或传入 -MSBuildPath。'
    }
    $candidates = @(& $vswherePath -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -find 'MSBuild\**\Bin\MSBuild.exe')
    if ($LASTEXITCODE -ne 0) { throw "vswhere 失败，退出码：$LASTEXITCODE" }
    $MSBuildPath = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($MSBuildPath) -or
    -not (Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
    throw '未找到 MSBuild。需要 Visual Studio 2022、v143 C++ 工具集和单项目 MSIX/WinUI 构建工具。'
}
$MSBuildPath = (Resolve-Path -LiteralPath $MSBuildPath).Path

if ([string]::IsNullOrWhiteSpace($WindowsSdkVersion)) {
    $kits = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
    $includePath = Join-Path $kits.KitsRoot10 'Include'
    $sdkCandidates = @(Get-ChildItem -LiteralPath $includePath -Directory |
        Where-Object {
            $_.Name -match '^10\.0\.\d+\.0$' -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'um/windows.h')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'winrt/windows.foundation.h'))
        } | Sort-Object { [version] $_.Name } -Descending)
    if ($sdkCandidates.Count -eq 0) { throw '未找到完整 Windows 10/11 SDK；请安装 SDK 或指定 -WindowsSdkVersion。' }
    $WindowsSdkVersion = $sdkCandidates[0].Name
}
if ($WindowsSdkVersion -notmatch '^10\.0\.\d+\.\d+$') {
    throw 'Windows SDK 版本必须使用 10.0.build.revision 格式。'
}

$commonArguments = @(
    $projectPath,
    '/nologo',
    '/m',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:WindowsTargetPlatformVersion=$WindowsSdkVersion"
)
$restoreArguments = $commonArguments + @(
    '/t:Restore',
    '/p:RestorePackagesConfig=true',
    "/p:RestoreRepositoryPath=$restorePath",
    "/p:RestoreConfigFile=$nugetConfigPath"
)
Write-Host "正在还原 NuGet 包（$Platform / $Configuration）……"
& $MSBuildPath @restoreArguments
if ($LASTEXITCODE -ne 0) { throw "NuGet 还原失败，退出码：$LASTEXITCODE" }

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$buildArguments = $commonArguments + @(
    '/t:Build',
    '/p:AppxPackageSigningEnabled=false',
    '/p:AppxBundle=Never',
    "/bl:$(Join-Path $outputRoot 'build.binlog')"
)
Write-Host "正在构建 Windows 原型，SDK $WindowsSdkVersion ……"
& $MSBuildPath @buildArguments
if ($LASTEXITCODE -ne 0) { throw "Windows 构建失败，退出码：$LASTEXITCODE" }

$manifestPath = Join-Path $layoutPath 'AppxManifest.xml'
$report = [ordered]@{
    schemaVersion = 1
    generatedAt = [DateTimeOffset]::Now.ToString('o')
    host = [Environment]::OSVersion.VersionString
    powershellVersion = $PSVersionTable.PSVersion.ToString()
    configuration = $Configuration
    platform = $Platform
    msbuildPath = $MSBuildPath
    windowsSdkVersion = $WindowsSdkVersion
    executablePath = Join-Path $outputRoot 'CPEditorNativeWindows.exe'
    layoutManifestPath = $manifestPath
    nativeBuildCompleted = $true
    launchRequested = $Launch.IsPresent
    activationRequested = $false
    uiValidationCompleted = $false
}
$reportPath = Join-Path $outputRoot 'build-result.json'
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $reportPath -Encoding utf8

if ($Launch) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "未生成 MSIX 布局清单：$manifestPath。请检查项目的 MSIX 构建工具和 build.binlog。"
    }
    # Framework-dependent deployment requires the matching Windows App SDK runtime.
    # Add-AppxPackage reports the missing dependency rather than hiding a deployment failure.
    # Windows 10 Appx cmdlets use their supported Windows PowerShell compatibility host.
    Import-Module Appx -UseWindowsPowerShell -ErrorAction Stop
    Add-AppxPackage -Register $manifestPath -ErrorAction Stop
    [xml] $manifest = Get-Content -LiteralPath $manifestPath -Raw
    $namespace = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
    $namespace.AddNamespace('p', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
    $identity = $manifest.SelectSingleNode('/p:Package/p:Identity', $namespace)
    $application = $manifest.SelectSingleNode('/p:Package/p:Applications/p:Application', $namespace)
    if ($null -eq $identity -or $null -eq $application) { throw '生成的 MSIX 清单缺少应用身份。' }
    $package = @(Get-AppxPackage -Name $identity.GetAttribute('Name')) |
        Where-Object { $_.Publisher -eq $identity.GetAttribute('Publisher') } |
        Sort-Object { [version] $_.Version } -Descending | Select-Object -First 1
    if ($null -eq $package) { throw '注册后未找到应用包。' }
    $activationUri = "shell:AppsFolder\$($package.PackageFamilyName)!$($application.GetAttribute('Id'))"
    Start-Process explorer.exe -ArgumentList $activationUri
    $report.activationRequested = $true
    $report.packageFamilyName = $package.PackageFamilyName
    Write-Host '已请求启动应用。启动请求不代表 UI、输入法或性能验收通过。'
}

$report | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Host "构建证据：$reportPath"

#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Get-CpnWindowsHost {
    $runtime = [Runtime.InteropServices.RuntimeInformation]
    $report = [ordered]@{
        osDescription = $runtime::OSDescription
        osArchitecture = $runtime::OSArchitecture.ToString()
        processArchitecture = $runtime::ProcessArchitecture.ToString()
        powershellVersion = $PSVersionTable.PSVersion.ToString()
        isWindows = $IsWindows
    }
    if ($IsWindows) {
        $os = Get-CimInstance Win32_OperatingSystem
        $report.caption = $os.Caption
        $report.version = $os.Version
        $report.buildNumber = $os.BuildNumber
        $report.operatingSystemSKU = $os.OperatingSystemSKU
        $report.targetWindows10_22H2Workstation = $os.BuildNumber -eq '19045' -and $os.OperatingSystemSKU -eq 161
    }
    return $report
}
function Get-CpnWindowsToolchain {
    [CmdletBinding()]
    param([ValidateSet('x64', 'arm64')][string] $Platform,
          [string] $MSBuildPath, [string] $CMakePath, [string] $NinjaPath, [string] $WindowsSdkVersion)
    if (-not $IsWindows) { throw '构建工具发现仅支持 Windows；当前主机不是 Windows。' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw '需要安装 Visual Studio 2022 和 vswhere。' }
    $installationsJson = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
    if ($LASTEXITCODE -ne 0) { throw "vswhere 失败：$LASTEXITCODE" }
    $installations = @($installationsJson -join "`n" | ConvertFrom-Json)
    if ($installations.Count -ne 1) { throw '没有找到完整的 Visual Studio 2022 C++ 工具安装。' }
    $installation = $installations[0]
    $vsRoot = $installation.installationPath
    if ([string]::IsNullOrWhiteSpace($WindowsSdkVersion)) {
        $kits = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
        $sdkDirectories = @(Get-ChildItem -LiteralPath (Join-Path $kits.KitsRoot10 'Include') -Directory |
            Where-Object { $_.Name -match '^10\.0\.\d+\.0$' -and
                (Test-Path -LiteralPath (Join-Path $_.FullName 'um/windows.h')) -and
                (Test-Path -LiteralPath (Join-Path $_.FullName 'winrt/windows.foundation.h')) } |
            Sort-Object { [version] $_.Name } -Descending)
        if (-not $sdkDirectories.Count) { throw '没有找到完整的 Windows SDK。' }
        $WindowsSdkVersion = $sdkDirectories[0].Name
    }
    if ($WindowsSdkVersion -notmatch '^10\.0\.\d+\.\d+$') { throw 'Windows SDK 版本格式无效。' }
    $devShell = Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path -LiteralPath $devShell -PathType Leaf)) { throw '缺少 Visual Studio Developer PowerShell 模块。' }
    Import-Module $devShell -ErrorAction Stop
    $targetArch = if ($Platform -eq 'x64') { 'amd64' } else { 'arm64' }
    $hostArch = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'amd64' }
    # DevCmdArguments is the supported module's validated option list, not an assembled shell command.
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation `
        -DevCmdArguments "-arch=$targetArch -host_arch=$hostArch -winsdk=$WindowsSdkVersion" | Out-Null
    if ([string]::IsNullOrWhiteSpace($MSBuildPath)) { $MSBuildPath = Join-Path $vsRoot 'MSBuild/Current/Bin/MSBuild.exe' }
    if ([string]::IsNullOrWhiteSpace($CMakePath)) {
        $bundledCMake = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
        $CMakePath = if (Test-Path -LiteralPath $bundledCMake -PathType Leaf) { $bundledCMake } else { (Get-Command cmake.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source }
    }
    if ([string]::IsNullOrWhiteSpace($NinjaPath)) {
        $bundledNinja = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
        if (Test-Path -LiteralPath $bundledNinja -PathType Leaf) { $NinjaPath = $bundledNinja }
        else { $foundNinja = Get-Command ninja.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1; if ($foundNinja) { $NinjaPath = $foundNinja.Source } }
    }
    foreach ($tool in @($MSBuildPath, $CMakePath)) {
        if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "工具路径不存在：$tool" }
    }
    $compiler = (Get-Command cl.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $ctest = Join-Path (Split-Path $CMakePath) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) { throw 'CMake 安装中缺少 ctest.exe。' }
    return [pscustomobject]@{
        vsInstallation = $vsRoot; vsVersion = $installation.installationVersion
        msbuild = $MSBuildPath; cmake = $CMakePath; ctest = $ctest; ninja = $NinjaPath
        compiler = $compiler; compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
        windowsSdkVersion = $WindowsSdkVersion
        generator = if ($NinjaPath) { 'Ninja' } else { 'Visual Studio 17 2022' }
    }
}
function Invoke-CpnNativeStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string] $Name, [Parameter(Mandatory)][string] $Executable,
          [string[]] $Arguments = @(), [Parameter(Mandatory)][System.Collections.IDictionary] $Report,
          [Parameter(Mandatory)][string] $OutputRoot)
    $logPath = Join-Path $OutputRoot "$Name.log"
    $started = [DateTimeOffset]::Now
    Write-Host "正在执行：$Name"
    $nativeExit = $null
    $invocationError = $null
    try {
        & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath | ForEach-Object { Write-Host $_ }
        $nativeExit = $LASTEXITCODE
    } catch { $invocationError = $_.Exception.Message; throw }
    finally {
        $completed = [DateTimeOffset]::Now
        $Report.stages.Add([ordered]@{
            name = $Name; executable = $Executable; arguments = $Arguments
            startedAt = $started.ToString('o'); completedAt = $completed.ToString('o')
            durationMilliseconds = ($completed - $started).TotalMilliseconds
            exitCode = $nativeExit; passed = $null -ne $nativeExit -and $nativeExit -eq 0
            invocationError = $invocationError; logPath = $logPath
        }) | Out-Null
    }
    if ($nativeExit -ne 0) { throw "$Name 失败，退出码：$nativeExit。日志：$logPath" }
}
function Get-CpnPackageHashes {
    param([Parameter(Mandatory)][string] $PackagesConfig, [Parameter(Mandatory)][string] $PackagesRoot)
    [xml] $packages = Get-Content -LiteralPath $PackagesConfig -Raw
    return @($packages.packages.package | ForEach-Object {
        $packageName = "$($_.id).$($_.version)"
        $archive = Join-Path (Join-Path $PackagesRoot $packageName) "$packageName.nupkg"
        [ordered]@{
            id = $_.id; version = $_.version; archivePath = $archive
            sha256 = if (Test-Path -LiteralPath $archive -PathType Leaf) { (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
        }
    })
}
Export-ModuleMember -Function Get-CpnWindowsHost, Get-CpnWindowsToolchain, Invoke-CpnNativeStage, Get-CpnPackageHashes

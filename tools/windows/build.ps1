#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string] $Configuration = 'Debug',
    [ValidateSet('x64', 'arm64')][string] $Platform = 'x64',
    [string] $MSBuildPath, [string] $CMakePath, [string] $NinjaPath, [string] $WindowsSdkVersion,
    [ValidateRange(1, 64)][int] $Parallelism = 4,
    [switch] $SkipCoreTests, [switch] $Deploy, [switch] $Launch
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Common.psm1') -Force
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputRoot = Join-Path $repositoryRoot "out/windows/$Platform/$Configuration"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$reportPath = Join-Path $outputRoot 'build-result.json'
$report = [ordered]@{
    schemaVersion = 3; generatedAt = [DateTimeOffset]::Now.ToString('o')
    host = Get-CpnWindowsHost; configuration = $Configuration; platform = $Platform
    stages = [Collections.Generic.List[object]]::new()
    coreBuildCompleted = $false; coreTestsExecuted = $false; coreTestsPassed = $false
    bridgeBuildCompleted = $false; bridgeTestsExecuted = $false; bridgeTestsPassed = $false
    sessionBuildCompleted = $false; sessionTestsExecuted = $false; sessionTestsPassed = $false
    sourceEditorBuildCompleted = $false; appBuildCompleted = $false
    deploymentRequested = $Deploy.IsPresent -or $Launch.IsPresent; deploymentCompleted = $false
    activationRequested = $false; uiValidationCompleted = $false; failure = $null
}
try {
    if (-not $IsWindows) { throw '此脚本仅支持 Windows 主机；当前主机的结果不能用作 Windows 验证。' }
    $toolchain = Get-CpnWindowsToolchain -Platform $Platform -MSBuildPath $MSBuildPath -CMakePath $CMakePath `
        -NinjaPath $NinjaPath -WindowsSdkVersion $WindowsSdkVersion
    $report.toolchain = $toolchain
    Invoke-CpnNativeStage -Name 'cmake-version' -Executable $toolchain.cmake -Arguments @('--version') -Report $report -OutputRoot $outputRoot
    Invoke-CpnNativeStage -Name 'msbuild-version' -Executable $toolchain.msbuild -Arguments @('/nologo', '/version') -Report $report -OutputRoot $outputRoot
    if ($toolchain.ninja) { Invoke-CpnNativeStage -Name 'ninja-version' -Executable $toolchain.ninja -Arguments @('--version') -Report $report -OutputRoot $outputRoot }
    $coreBuild = Join-Path $outputRoot 'core'
    $configure = @('-S', $repositoryRoot, '-B', $coreBuild, '-G', $toolchain.generator, '-DBUILD_TESTING=ON')
    if ($toolchain.generator -eq 'Ninja') {
        $configure += @("-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_MAKE_PROGRAM=$($toolchain.ninja)",
                        "-DCMAKE_CXX_COMPILER=$($toolchain.compiler)", "-DCMAKE_C_COMPILER=$($toolchain.compiler)")
    } else {
        $cmakePlatform = if ($Platform -eq 'arm64') { 'ARM64' } else { 'x64' }
        $configure += @('-A', $cmakePlatform, "-DCMAKE_SYSTEM_VERSION=$($toolchain.windowsSdkVersion)")
    }
    Invoke-CpnNativeStage -Name 'core-configure' -Executable $toolchain.cmake -Arguments $configure -Report $report -OutputRoot $outputRoot
    Invoke-CpnNativeStage -Name 'core-build' -Executable $toolchain.cmake `
        -Arguments @('--build', $coreBuild, '--config', $Configuration, '--parallel', "$Parallelism") -Report $report -OutputRoot $outputRoot
    $report.coreBuildCompleted = $true
    $coreLibraryDir = if ($toolchain.generator -eq 'Ninja') { $coreBuild } else { Join-Path $coreBuild $Configuration }
    $coreLibrary = Join-Path $coreLibraryDir 'cpn_core.lib'
    if (-not (Test-Path -LiteralPath $coreLibrary -PathType Leaf)) { throw "未生成共享 Core 归档：$coreLibrary" }
    $report.coreLibrary = [ordered]@{ path = $coreLibrary; sha256 = (Get-FileHash -LiteralPath $coreLibrary -Algorithm SHA256).Hash.ToLowerInvariant() }
    Invoke-CpnNativeStage -Name 'ctest-plan' -Executable $toolchain.ctest `
        -Arguments @('--test-dir', $coreBuild, '-C', $Configuration, '--show-only=json-v1') -Report $report -OutputRoot $outputRoot
    $testPlan = Get-Content -LiteralPath (Join-Path $outputRoot 'ctest-plan.log') -Raw | ConvertFrom-Json
    $report.coreTestNames = @($testPlan.tests | ForEach-Object { $_.name })
    if ($report.coreTestNames.Count -eq 0) { throw 'CTest 计划为空；不能把没有执行测试报告为通过。' }
    $nativeTestPlatform = $report.host.osArchitecture -eq $Platform -or ($Platform -eq 'x64' -and $report.host.osArchitecture -eq 'X64')
    if (-not $SkipCoreTests -and $nativeTestPlatform) {
        $report.coreTestsExecuted = $true
        Invoke-CpnNativeStage -Name 'ctest' -Executable $toolchain.ctest `
            -Arguments @('--test-dir', $coreBuild, '-C', $Configuration, '--output-on-failure', '--no-tests=error') -Report $report -OutputRoot $outputRoot
        $report.coreTestsPassed = $true
    } else {
        $report.coreTestsSkippedReason = if ($SkipCoreTests) { '用户指定 SkipCoreTests；没有执行测试。' } else { '输出架构与执行主机不一致；未将交叉编译报告为运行验证。' }
    }
    $stateTestExe = Join-Path $outputRoot 'committed-text-tests.exe'
    Invoke-CpnNativeStage -Name 'committed-state-build' -Executable $toolchain.compiler -Arguments @(
        '/nologo', '/std:c++20', '/EHsc', '/W4', '/utf-8',
        (Join-Path $repositoryRoot 'patches/winuiedit/tests/committed_text_tests.cpp'),
        "/Fo:$(Join-Path $outputRoot 'committed-text-tests.obj')", "/Fe:$stateTestExe"
    ) -Report $report -OutputRoot $outputRoot
    if ($nativeTestPlatform) { Invoke-CpnNativeStage -Name 'committed-state-test' -Executable $stateTestExe -Report $report -OutputRoot $outputRoot }
    $bridgeTestExe = Join-Path $outputRoot 'core-adapter-tests.exe'
    $bridgeObjects = Join-Path $outputRoot 'bridge-tests/obj'
    New-Item -ItemType Directory -Path $bridgeObjects -Force | Out-Null
    $bridgeSource = Join-Path $repositoryRoot 'apps/windows/CPEditorNativeWindows'
    $runtimeLibrary = if ($Configuration -eq 'Debug') { '/MDd' } else { '/MD' }
    Invoke-CpnNativeStage -Name 'bridge-build' -Executable $toolchain.compiler -Arguments @(
        '/nologo', '/std:c++20', '/EHsc', '/W4', '/utf-8', $runtimeLibrary, '/DCPN_PORTABLE_ADAPTER_TEST',
        "/I$(Join-Path $repositoryRoot 'core/include')", "/I$(Join-Path $repositoryRoot 'third_party/json/include')", "/I$bridgeSource",
        (Join-Path $bridgeSource 'CoreWire.cpp'), (Join-Path $bridgeSource 'CoreRunner.cpp'),
        (Join-Path $bridgeSource 'RunnerConfiguration.cpp'), (Join-Path $repositoryRoot 'apps/windows/tests/CoreRunnerTests.cpp'),
        "/Fo$bridgeObjects\", "/Fe:$bridgeTestExe", '/link', $coreLibrary
    ) -Report $report -OutputRoot $outputRoot
    $report.bridgeBuildCompleted = $true
    if ($nativeTestPlatform) {
        $report.bridgeTestsExecuted = $true
        Invoke-CpnNativeStage -Name 'bridge-tests' -Executable $bridgeTestExe `
            -Arguments @((Join-Path $bridgeSource 'runner-settings.json')) -Report $report -OutputRoot $outputRoot
        $report.bridgeTestsPassed = $true
    }
    $sessionTestExe = Join-Path $outputRoot 'session-tests.exe'
    $sessionObjects = Join-Path $outputRoot 'session-tests/obj'
    New-Item -ItemType Directory -Path $sessionObjects -Force | Out-Null
    Invoke-CpnNativeStage -Name 'session-build' -Executable $toolchain.compiler -Arguments @(
        '/nologo', '/std:c++20', '/EHsc', '/W4', '/utf-8', $runtimeLibrary, '/DCPN_PORTABLE_ADAPTER_TEST',
        "/I$(Join-Path $repositoryRoot 'core/include')", "/I$(Join-Path $repositoryRoot 'third_party/json/include')", "/I$bridgeSource",
        (Join-Path $bridgeSource 'SessionWorker.cpp'), (Join-Path $bridgeSource 'RecoveryModel.cpp'),
        (Join-Path $bridgeSource 'SourceStore.cpp'), (Join-Path $bridgeSource 'RunnerConfiguration.cpp'),
        (Join-Path $repositoryRoot 'apps/windows/tests/SessionTests.cpp'),
        "/Fo$sessionObjects\", "/Fe:$sessionTestExe", '/link', $coreLibrary
    ) -Report $report -OutputRoot $outputRoot
    $report.sessionBuildCompleted = $true
    if ($nativeTestPlatform) {
        $report.sessionTestsExecuted = $true
        Invoke-CpnNativeStage -Name 'session-tests' -Executable $sessionTestExe `
            -Arguments @((Join-Path $bridgeSource 'recovery-settings.json')) -Report $report -OutputRoot $outputRoot
        $report.sessionTestsPassed = $true
    } else {
        $report.sessionTestsSkippedReason = '输出架构与执行主机不一致；会话桥接仅完成构建，未执行。'
    }
    $sourceLockPath = Join-Path $repositoryRoot 'patches/winuiedit/source-lock.json'
    $sourceLock = Get-Content -LiteralPath $sourceLockPath -Raw | ConvertFrom-Json
    $sourceLockHash = (Get-FileHash -LiteralPath $sourceLockPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $sourceIdentity = "$($sourceLock.source_commit.Substring(0, 12))-$($sourceLockHash.Substring(0, 12))"
    $prepared = & (Join-Path $PSScriptRoot 'prepare-winuiedit.ps1') -OutputRoot (Join-Path $repositoryRoot "out/windows/dependencies/winuiedit/$sourceIdentity")
    $report.preparedEditor = $prepared
    $nugetConfig = Join-Path $repositoryRoot 'apps/windows/NuGet.Config'
    $sourcePackages = Join-Path $prepared.root 'packages'
    $editorBinary = Join-Path $outputRoot 'editor/bin'
    $editorObject = Join-Path $outputRoot 'editor/obj'
    $editorGenerated = Join-Path $editorObject 'Generated Files'
    $sourceArguments = @($prepared.project, '/nologo', "/m:$Parallelism", "/p:Configuration=$Configuration",
        "/p:Platform=$Platform", '/p:WinUIEditorUseWinUI3=true', '/p:GenerateProjectSpecificOutputFolder=false',
        "/p:WindowsTargetPlatformVersion=$($toolchain.windowsSdkVersion)",
        "/p:SolutionDir=$($prepared.root)\", "/p:OutDir=$editorBinary\", "/p:IntDir=$editorObject\", "/p:GeneratedFilesDir=$editorGenerated\")
    Invoke-CpnNativeStage -Name 'editor-restore' -Executable $toolchain.msbuild -Arguments ($sourceArguments + @(
        '/t:Restore', '/p:RestorePackagesConfig=true', "/p:RestoreRepositoryPath=$sourcePackages", "/p:RestoreConfigFile=$nugetConfig"
    )) -Report $report -OutputRoot $outputRoot
    # Restore 完成后独立求值；日志只含属性 JSON，不与编译输出混合。
    Invoke-CpnNativeStage -Name 'editor-output-properties' -Executable $toolchain.msbuild -Arguments ($sourceArguments + @(
        '/getProperty:OutDir,TargetDir,TargetPath,GenerateProjectSpecificOutputFolder'
    )) -Report $report -OutputRoot $outputRoot
    $report.editorOutputProperties = Read-CpnMSBuildOutputProperties -LogPath (Join-Path $outputRoot 'editor-output-properties.log') `
        -ExpectedTargetDir $editorBinary -ExpectedTargetPath (Join-Path $editorBinary 'WinUIEditor.dll')
    Invoke-CpnNativeStage -Name 'editor-build' -Executable $toolchain.msbuild -Arguments ($sourceArguments + @('/t:Build', "/bl:$(Join-Path $outputRoot 'editor-build.binlog')")) -Report $report -OutputRoot $outputRoot
    $report.sourceEditorBuildCompleted = $true
    $report.editorPackageHashes = Get-CpnPackageHashes -PackagesConfig (Join-Path $prepared.root 'WinUIEditor/packages.config') -PackagesRoot $sourcePackages
    foreach ($artifact in @('WinUIEditor.dll', 'WinUIEditor.winmd', 'WinUIEditor.pri')) {
        if (-not (Test-Path -LiteralPath (Join-Path $editorBinary $artifact) -PathType Leaf)) { throw "控件构建产物缺失：$artifact" }
    }
    $report.editorArtifacts = @('WinUIEditor.dll', 'WinUIEditor.winmd', 'WinUIEditor.pri') | ForEach-Object {
        $artifactPath = Join-Path $editorBinary $_
        [ordered]@{ path = $artifactPath; sha256 = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    # 将发现的 MSVC 编译环境写入配置，供从 Explorer 启动的应用使用。
    $generatedToolchains = Join-Path $outputRoot 'toolchains'
    New-Item -ItemType Directory -Path $generatedToolchains -Force | Out-Null
    $msvcProfile = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'profiles/cpp-msvc.json') -Raw | ConvertFrom-Json
    $msvcProfile.build.executable = $toolchain.compiler
    $environmentConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'msvc-environment.json') -Raw | ConvertFrom-Json
    $msvcProfile.environment = @($environmentConfig.variables | ForEach-Object {
        $value = [Environment]::GetEnvironmentVariable($_, 'Process')
        if ($null -ne $value) { [pscustomobject]@{ name = $_; value = $value } }
    })
    $msvcProfile | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $generatedToolchains 'cpp-msvc.json') -Encoding utf8
    $appProject = Join-Path $repositoryRoot 'apps/windows/CPEditorNativeWindows/CPEditorNativeWindows.vcxproj'
    $appPackages = Join-Path $repositoryRoot 'apps/windows/packages'
    $appExecutable = Join-Path $outputRoot 'CPEditorNativeWindows.exe'
    $appLayout = Join-Path $outputRoot 'layout'
    $appArguments = @($appProject, '/nologo', "/m:$Parallelism", "/p:Configuration=$Configuration", "/p:Platform=$Platform",
        '/p:WinUIEditorUseWinUI3=true', '/p:GenerateProjectSpecificOutputFolder=false',
        "/p:WindowsTargetPlatformVersion=$($toolchain.windowsSdkVersion)",
        "/p:CPNWinUIEditProject=$($prepared.project)", "/p:CPNEditorBinaryDir=$editorBinary\",
        "/p:CPNEditorObjectDir=$editorObject\", "/p:CPNEditorGeneratedDir=$editorGenerated\",
        "/p:CPNPreparedEditorRoot=$($prepared.root)\", "/p:CPNCoreLibraryDir=$coreLibraryDir",
        "/p:CPNGeneratedToolchainsDir=$generatedToolchains", '/p:AppxPackageSigningEnabled=false', '/p:AppxBundle=Never')
    Invoke-CpnNativeStage -Name 'app-restore' -Executable $toolchain.msbuild -Arguments ($appArguments + @(
        '/t:Restore', '/p:RestorePackagesConfig=true', "/p:RestoreRepositoryPath=$appPackages", "/p:RestoreConfigFile=$nugetConfig"
    )) -Report $report -OutputRoot $outputRoot
    Invoke-CpnNativeStage -Name 'app-output-properties' -Executable $toolchain.msbuild -Arguments ($appArguments + @(
        '/getProperty:OutDir,TargetDir,TargetPath,AppxLayoutDir,GenerateProjectSpecificOutputFolder'
    )) -Report $report -OutputRoot $outputRoot
    $report.appOutputProperties = Read-CpnMSBuildOutputProperties -LogPath (Join-Path $outputRoot 'app-output-properties.log') `
        -ExpectedTargetDir $outputRoot -ExpectedTargetPath $appExecutable -ExpectedAppxLayoutDir $appLayout
    Invoke-CpnNativeStage -Name 'app-build' -Executable $toolchain.msbuild -Arguments ($appArguments + @('/t:Build', "/bl:$(Join-Path $outputRoot 'app-build.binlog')")) -Report $report -OutputRoot $outputRoot
    if (-not (Test-Path -LiteralPath $appExecutable -PathType Leaf)) { throw "原生应用构建产物缺失：$appExecutable" }
    $report.appBuildCompleted = $true
    $report.appPackageHashes = Get-CpnPackageHashes -PackagesConfig (Join-Path (Split-Path $appProject) 'packages.config') -PackagesRoot $appPackages
    $manifestPath = Join-Path $appLayout 'AppxManifest.xml'
    $report.layoutManifestPath = $manifestPath
    $report.executablePath = $appExecutable
    if ($Deploy -or $Launch) {
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "缺少 MSIX layout 清单：$manifestPath" }
        Import-Module Appx -UseWindowsPowerShell -ErrorAction Stop
        Add-AppxPackage -Register $manifestPath -ErrorAction Stop
        $report.deploymentCompleted = $true
        [xml] $manifest = Get-Content -LiteralPath $manifestPath -Raw
        $namespace = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
        $namespace.AddNamespace('p', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
        $identity = $manifest.SelectSingleNode('/p:Package/p:Identity', $namespace)
        $application = $manifest.SelectSingleNode('/p:Package/p:Applications/p:Application', $namespace)
        $package = @(Get-AppxPackage -Name $identity.GetAttribute('Name')) |
            Where-Object { $_.Publisher -eq $identity.GetAttribute('Publisher') } |
            Sort-Object { [version] $_.Version } -Descending | Select-Object -First 1
        if ($null -eq $package) { throw '注册后未找到应用包。' }
        $report.packageFamilyName = $package.PackageFamilyName
        if ($Launch) {
            Start-Process explorer.exe -ArgumentList "shell:AppsFolder\$($package.PackageFamilyName)!$($application.GetAttribute('Id'))"
            $report.activationRequested = $true
            Write-Host '已请求启动；此状态不代表 UI、IME 或性能验收通过。'
        }
    }
} catch { $report.failure = $_.Exception.Message; throw }
finally {
    $report.completedAt = [DateTimeOffset]::Now.ToString('o')
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Host "构建报告：$reportPath"
}

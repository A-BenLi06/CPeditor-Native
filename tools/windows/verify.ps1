#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string] $Configuration = 'Debug',
    [ValidateSet('x64', 'arm64')][string] $Platform = 'x64',
    [string] $MSBuildPath, [string] $CMakePath, [string] $NinjaPath, [string] $WindowsSdkVersion,
    [ValidateRange(1, 64)][int] $Parallelism = 4,
    [switch] $Deploy, [switch] $Launch
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'Common.psm1') -Force
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputRoot = Join-Path $repositoryRoot "out/windows/$Platform/$Configuration"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$definition = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'verification-scenarios.json') -Raw | ConvertFrom-Json
if ($definition.schema_version -ne 1) { throw '不支持的实测场景契约版本。' }
$reportPath = Join-Path $outputRoot 'verification-report.json'
$report = [ordered]@{
    schemaVersion = 1; generatedAt = [DateTimeOffset]::Now.ToString('o')
    host = Get-CpnWindowsHost; configuration = $Configuration; platform = $Platform
    automatedExecutionCompleted = $false; automatedPassed = $false
    uiValidationCompleted = $false; activationRequested = $false; failure = $null
    manualScenarios = @($definition.scenarios | ForEach-Object {
        [ordered]@{ id = $_.id; name = $_.name; method = $_.method; expected = $_.expected
            status = 'not_run'; startedAt = $null; completedAt = $null; passed = $null
            observations = ''; evidenceRequirements = $_.evidence; evidenceFiles = @() }
    })
}
$templatePath = Join-Path $outputRoot 'measurement-template.md'
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('# Windows 原生应用实测记录')
$lines.Add('')
$lines.Add("生成时间：$($report.generatedAt)。自动步骤与手工实测分别记录；启动请求不等于验证通过。")
$lines.Add('')
$lines.Add('当前每项默认“未执行”。仅在目标主机完成操作并附证据后填写结果。以下为验收期望，不是已实现/已验证行为的声明。')
foreach ($scenario in $definition.scenarios) {
    $lines.Add(''); $lines.Add("## $($scenario.name)"); $lines.Add('')
    $lines.Add("场景 ID：$($scenario.id)"); $lines.Add('状态：未执行')
    $lines.Add("方法：$($scenario.method)"); $lines.Add("期望：$($scenario.expected)")
    $lines.Add("所需证据：$($scenario.evidence -join '、')")
    $lines.Add('开始时间（ISO 8601，包含 UTC 偏移）：')
    $lines.Add('结束时间（ISO 8601，包含 UTC 偏移）：')
    $lines.Add('实际观察：'); $lines.Add('结果（通过/失败/未执行）：'); $lines.Add('证据文件与测量方法：')
}
$lines | Set-Content -LiteralPath $templatePath -Encoding utf8
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding utf8
try {
    $buildArguments = @{
        Configuration = $Configuration; Platform = $Platform; Parallelism = $Parallelism
        MSBuildPath = $MSBuildPath; CMakePath = $CMakePath; NinjaPath = $NinjaPath
        WindowsSdkVersion = $WindowsSdkVersion; Deploy = $Deploy; Launch = $Launch
    }
    & (Join-Path $PSScriptRoot 'build.ps1') @buildArguments
    $buildReport = Get-Content -LiteralPath (Join-Path $outputRoot 'build-result.json') -Raw | ConvertFrom-Json
    $report.buildReportPath = Join-Path $outputRoot 'build-result.json'
    $report.automatedExecutionCompleted = $true
    $report.automatedPassed = $buildReport.coreBuildCompleted -and $buildReport.coreTestsExecuted -and
        $buildReport.coreTestsPassed -and $buildReport.sourceEditorBuildCompleted -and $buildReport.appBuildCompleted
    $report.activationRequested = $buildReport.activationRequested
    $report.deploymentCompleted = $buildReport.deploymentCompleted
} catch {
    $report.failure = $_.Exception.Message
    $buildReportPath = Join-Path $outputRoot 'build-result.json'
    if (Test-Path -LiteralPath $buildReportPath -PathType Leaf) { $report.buildReportPath = $buildReportPath }
    throw
} finally {
    $report.completedAt = [DateTimeOffset]::Now.ToString('o')
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Host "验证报告：$reportPath"
    Write-Host "手工实测模板：$templatePath"
}

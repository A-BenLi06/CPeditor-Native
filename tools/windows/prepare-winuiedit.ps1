#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string] $OutputRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$lockDirectory = Join-Path $repositoryRoot 'patches/winuiedit'
$lockPath = Join-Path $lockDirectory 'source-lock.json'
$sourceLock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($sourceLock.schema_version -ne 1) { throw '不支持的 WinUIEdit source-lock 版本。' }
$patchPath = Join-Path $lockDirectory $sourceLock.patch
if ((Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sourceLock.patch_sha256) {
    throw 'WinUIEdit 补丁 SHA256 不匹配。'
}
$sourcePath = Join-Path $repositoryRoot 'third_party/WinUIEdit'
$git = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$actualCommit = & $git -C $sourcePath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actualCommit.Trim() -ne $sourceLock.source_commit) {
    throw '参考子模块的源码版本与 source-lock 不一致；请先初始化固定子模块。'
}
$preparedRoot = [IO.Path]::GetFullPath($OutputRoot)
$receiptPath = Join-Path $preparedRoot '.cpn-prepared-source.json'
function Assert-PreparedSource {
    param([object] $Receipt)
    if ($Receipt.sourceCommit -ne $sourceLock.source_commit -or $Receipt.patchSha256 -ne $sourceLock.patch_sha256) {
        throw '构建副本身份与当前 source-lock 不一致。请使用新的输出目录。'
    }
    if ($Receipt.sourceLockSha256 -ne (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw '构建副本的完整 source-lock 身份已变化；请使用新的输出目录。'
    }
    foreach ($entry in $Receipt.files) {
        $path = Join-Path $preparedRoot $entry.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "构建副本文件发生变化：$($entry.path)。未覆盖这些修改。"
        }
    }
    foreach ($entry in $sourceLock.files) {
        $path = Join-Path $preparedRoot $entry.path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.after_sha256) {
            throw "补丁产物 SHA256 不匹配：$($entry.path)"
        }
    }
    foreach ($entry in $sourceLock.overlays) {
        if ((Get-FileHash -LiteralPath (Join-Path $preparedRoot $entry.target) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "状态机 overlay SHA256 不匹配：$($entry.target)"
        }
    }
}
if (Test-Path -LiteralPath $preparedRoot) {
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw '输出目录已存在但没有构建副本收据；请指定新的空目录。'
    }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    Assert-PreparedSource $receipt
} else {
    New-Item -ItemType Directory -Path $preparedRoot | Out-Null
    $archivePath = "$preparedRoot.zip"
    try {
        & $git -C $sourcePath archive --format=zip "--output=$archivePath" $sourceLock.source_commit
        if ($LASTEXITCODE -ne 0) { throw "git archive 失败，退出码：$LASTEXITCODE" }
        Expand-Archive -LiteralPath $archivePath -DestinationPath $preparedRoot -Force
        foreach ($entry in $sourceLock.files) {
            if ((Get-FileHash -LiteralPath (Join-Path $preparedRoot $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.before_sha256) {
                throw "固定源码 SHA256 不匹配：$($entry.path)"
            }
        }
        # A private empty repository establishes the apply root; reference submodule stays untouched.
        & $git -C $preparedRoot init --quiet
        if ($LASTEXITCODE -ne 0) { throw "准备 Git apply 根目录失败：$LASTEXITCODE" }
        & $git -C $preparedRoot -c core.whitespace=cr-at-eol apply --check --whitespace=error-all $patchPath
        if ($LASTEXITCODE -ne 0) { throw "补丁上下文校验失败：$LASTEXITCODE" }
        & $git -C $preparedRoot -c core.whitespace=cr-at-eol apply --whitespace=error-all $patchPath
        if ($LASTEXITCODE -ne 0) { throw "补丁应用失败：$LASTEXITCODE" }
        foreach ($entry in $sourceLock.overlays) {
            $overlayPath = Join-Path $lockDirectory $entry.source
            if ((Get-FileHash -LiteralPath $overlayPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
                throw "源 overlay SHA256 不匹配：$($entry.source)"
            }
            Copy-Item -LiteralPath $overlayPath -Destination (Join-Path $preparedRoot $entry.target)
        }
        $files = @(Get-ChildItem -LiteralPath $preparedRoot -Recurse -File | ForEach-Object {
            $relative = [IO.Path]::GetRelativePath($preparedRoot, $_.FullName).Replace('\', '/')
            if (-not $relative.StartsWith('.git/')) {
                [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
            }
        } | Sort-Object { $_.path })
        $receipt = [ordered]@{
            schemaVersion = 1
            generatedAt = [DateTimeOffset]::Now.ToString('o')
            sourceCommit = $sourceLock.source_commit
            patchSha256 = $sourceLock.patch_sha256
            sourceLockSha256 = (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant()
            files = $files
        }
        Assert-PreparedSource $receipt
        $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding utf8
    } finally {
        if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath }
    }
}
[pscustomobject]@{
    root = $preparedRoot
    project = Join-Path $preparedRoot 'WinUIEditor/WinUIEditor.vcxproj'
    receipt = $receiptPath
    sourceCommit = $sourceLock.source_commit
    patchSha256 = $sourceLock.patch_sha256
    sourceLockSha256 = (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant()
}

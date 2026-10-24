#Requires -Version 5.1
<#
.SYNOPSIS
  Build Release artifacts for PScript (Windows x64).

.DESCRIPTION
  Produces dist/pscript-<version>-windows-x64/ with bin/, docs/, LICENSE, README,
  RELEASE_METADATA.json, SHA256SUMS.txt, and a .zip archive. Also builds a source
  distribution zip (no build/, node_modules/, .git/).

  Usage:
    .\scripts\package_release.ps1 [-Configuration Release] [-BuildDir build] [-SkipBuild]
#>
param(
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string] $Configuration = 'Release',
    [string] $BuildDir = 'build',
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $RepoRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }

function Get-VersionString {
    $cmake = Get-Content (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(PScript VERSION ([0-9.]+)') { throw 'Could not parse project VERSION' }
    $base = $Matches[1]
    if ($cmake -match 'set\(PSCRIPT_VERSION_PRERELEASE "([^"]*)"') {
        $pre = $Matches[1]
    } else {
        $pre = ''
    }
    if ($pre -eq '') { return $base }
    return "$base-$pre"
}

$Version = Get-VersionString
$PlatformTag = 'windows-x64'
$DistName = "pscript-$Version-$PlatformTag"
$DistRoot = Join-Path $RepoRoot "dist\$DistName"
$BinDir = Join-Path $DistRoot 'bin'
$DocsDir = Join-Path $DistRoot 'docs'

if (-not $SkipBuild) {
    Write-Host "Configuring CMake..."
    cmake -S $RepoRoot -B $BuildPath | Out-Null
    Write-Host "Building Release targets (pscript, pscript_lsp)..."
    cmake --build $BuildPath --config $Configuration --target pscript pscript_lsp
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
}

$PscriptExe = Join-Path $BuildPath 'pscript.exe'
$LspExe = Join-Path $BuildPath 'pscript_lsp.exe'
foreach ($exe in @($PscriptExe, $LspExe)) {
    if (-not (Test-Path $exe)) { throw "Missing $exe - build Release first" }
}

if (Test-Path $DistRoot) { Remove-Item -Recurse -Force $DistRoot }
New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
New-Item -ItemType Directory -Path $DocsDir -Force | Out-Null

Copy-Item $PscriptExe $BinDir
Copy-Item $LspExe $BinDir
Copy-Item (Join-Path $RepoRoot 'LICENSE') $DistRoot
Copy-Item (Join-Path $RepoRoot 'README.md') $DistRoot
Copy-Item (Join-Path $RepoRoot 'CHANGELOG.md') $DistRoot

$DocFiles = @(
    'docs/COMPATIBILITY.md',
    'docs/INSTALLATION.md',
    'docs/GETTING_STARTED.md',
    'docs/TROUBLESHOOTING.md',
    'docs/MIGRATION.md',
    'docs/package-ecosystem.md',
    'docs/LANGUAGE_SPEC.md',
    'docs/language/developer-cli.md',
    'docs/language/stdlib.md',
    'docs/language/editor-integration.md',
    'docs/release/RELEASE_NOTES-1.0.0.md'
)
foreach ($rel in $DocFiles) {
    $src = Join-Path $RepoRoot $rel
    if (Test-Path $src) {
        $dest = Join-Path $DocsDir ($rel -replace '^docs/', '')
        $destDir = Split-Path $dest -Parent
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item $src $dest
    }
}

Copy-Item -Recurse (Join-Path $RepoRoot 'examples') (Join-Path $DistRoot 'examples')

$VersionOut = & (Join-Path $BinDir 'pscript.exe') --version 2>&1 | Out-String
$VersionOut = $VersionOut.Trim()
if ($VersionOut -ne $Version) {
    throw "pscript --version returned '$VersionOut', expected '$Version'"
}

$JitProbe = @'
func f():
    return 1 + 2
print(f())
'@
$ProbeFile = Join-Path $env:TEMP "pscript_jit_probe_$PID.phs"
Set-Content -Path $ProbeFile -Value $JitProbe -Encoding UTF8
$RunOut = & (Join-Path $BinDir 'pscript.exe') $ProbeFile 2>&1 | Out-String
Remove-Item $ProbeFile -ErrorAction SilentlyContinue
if ($RunOut.Trim() -ne '3') { throw "Smoke run failed: $RunOut" }

$UtcNow = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$ArtifactList = @(
    'bin/pscript.exe',
    'bin/pscript_lsp.exe',
    'LICENSE',
    'README.md',
    'CHANGELOG.md',
    'examples/hello/main.phs'
)
$Metadata = [ordered]@{
    name = 'pscript'
    version = $Version
    semver_base = ($Version -replace '-.*', '')
    platform = $PlatformTag
    configuration = $Configuration
    jit_backend = 'x64-native'
    built_utc = $UtcNow
    artifacts = $ArtifactList
}
$Metadata | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $DistRoot 'RELEASE_METADATA.json') -Encoding UTF8

$Hashes = @()
Get-ChildItem -Path $DistRoot -Recurse -File | ForEach-Object {
    $hash = Get-FileHash -Path $_.FullName -Algorithm SHA256
    $rel = $_.FullName.Substring($DistRoot.Length + 1) -replace '\\', '/'
    $Hashes += "$($hash.Hash.ToLower())  $rel"
}
$Hashes | Set-Content (Join-Path $DistRoot 'SHA256SUMS.txt') -Encoding ASCII

$DistZip = Join-Path $RepoRoot "dist\$DistName.zip"
if (Test-Path $DistZip) { Remove-Item $DistZip -Force }
Compress-Archive -Path $DistRoot -DestinationPath $DistZip -CompressionLevel Optimal
$ZipHash = (Get-FileHash -Path $DistZip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$DistZip.sha256" -Value "$ZipHash  $(Split-Path $DistZip -Leaf)" -Encoding ASCII

# Source distribution
$SourceZip = Join-Path $RepoRoot "dist\pscript-$Version-source.zip"
if (Test-Path $SourceZip) { Remove-Item $SourceZip -Force }
$SourceStage = Join-Path $RepoRoot "dist\_source_stage\pscript-$Version-source"
if (Test-Path $SourceStage) { Remove-Item -Recurse -Force $SourceStage }
New-Item -ItemType Directory -Path $SourceStage -Force | Out-Null

function Copy-TreeFiltered {
    param([string]$Src, [string]$Dst)
    $ExcludeNames = @('build', 'Build', 'dist', '.git', 'vcpkg_installed', 'node_modules', 'Testing', '.cache', '.vscode-test')
    Get-ChildItem $Src -Force | Where-Object {
        $_.Name -notin $ExcludeNames -and $_.Name -notlike 'build-*'
    } | ForEach-Object {
        $target = Join-Path $Dst $_.Name
        if ($_.PSIsContainer) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
            Copy-TreeFiltered -Src $_.FullName -Dst $target
        } else {
            Copy-Item $_.FullName -Destination $target -Force
        }
    }
}
Copy-TreeFiltered -Src $RepoRoot -Dst $SourceStage
Compress-Archive -Path $SourceStage -DestinationPath $SourceZip -CompressionLevel Optimal
$SrcHash = (Get-FileHash -Path $SourceZip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$SourceZip.sha256" -Value "$SrcHash  $(Split-Path $SourceZip -Leaf)" -Encoding ASCII
Remove-Item -Recurse -Force (Split-Path $SourceStage -Parent)

Write-Host "Release package ready:"
Write-Host "  $DistRoot"
Write-Host "  $DistZip"
Write-Host "  $SourceZip"
Write-Host "Version: $Version (verified via pscript --version)"

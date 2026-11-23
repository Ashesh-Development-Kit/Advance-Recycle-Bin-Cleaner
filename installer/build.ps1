[CmdletBinding()]
param([string]$MakeNsis)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$zig = (Get-Command zig -CommandType Application | Select-Object -First 1).Source

if (-not $MakeNsis) {
    $installed = Get-Command makensis -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($installed) {
        $MakeNsis = $installed.Source
    } else {
        $tools = Join-Path $root '.zig-cache\installer-tools'
        $MakeNsis = Join-Path $tools 'nsis-3.12\makensis.exe'
        if (-not (Test-Path -LiteralPath $MakeNsis -PathType Leaf)) {
            New-Item -ItemType Directory -Force -Path $tools | Out-Null
            $archive = Join-Path $tools 'nsis-3.12.zip'
            $expected = '56581F90DB321581C5381193D796FFFCF2D24B2F8FED2160A6C6A3BAA67F2C4F'
            $mirrors = @(
                'https://downloads.sourceforge.net/project/nsis/NSIS%203/3.12/nsis-3.12.zip',
                'https://distfiles.macports.org/nsis/nsis-3.12.zip'
            )
            $verified = $false
            foreach ($uri in $mirrors) {
                try {
                    Invoke-WebRequest -Uri $uri -OutFile $archive -TimeoutSec 30
                    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -eq $expected) {
                        $verified = $true
                        break
                    }
                    Write-Warning "The NSIS download from $uri did not match its published SHA256."
                } catch {
                    Write-Warning "Could not download NSIS from $uri. $($_.Exception.Message)"
                }
            }
            if (-not $verified) { throw 'Could not obtain a verified NSIS archive. Pass -MakeNsis with an existing compiler path.' }
            Expand-Archive -LiteralPath $archive -DestinationPath $tools -Force
        }
    }
}
$MakeNsis = (Resolve-Path -LiteralPath $MakeNsis).Path

Push-Location -LiteralPath $root
try {
    $staging = Join-Path $root '.zig-cache\installer-payload'
    & $zig build -Dtarget=x86_64-windows-gnu -Doptimize=ReleaseSafe --prefix $staging --summary all
    if ($LASTEXITCODE -ne 0) { throw "Zig build failed with exit code $LASTEXITCODE." }

    $payload = Join-Path $staging 'bin'
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $payload 'ARBC.exe'))
    if ($version.FileVersion -notmatch '^\d+(\.\d+){3}(-[a-zA-Z0-9-]+)?$' -or
        $version.ProductVersion -notmatch '^\d+(\.\d+){3}(-[a-zA-Z0-9-]+)?$') {
        throw 'ARBC.exe has missing or invalid version metadata.'
    }
    $fileNumeric = @($version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart, $version.FilePrivatePart) -join '.'
    $productNumeric = @($version.ProductMajorPart, $version.ProductMinorPart, $version.ProductBuildPart, $version.ProductPrivatePart) -join '.'
    $output = Join-Path $root 'zig-out\installer'
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $setupName = "ARBC-Setup-$($version.FileVersion)-x64.exe"
    $setup = Join-Path $output $setupName
    $arguments = @(
        '/V3', "/DPAYLOAD_DIR=$payload", "/DSETUP_PATH=$setup", "/DSETUP_NAME=$setupName"
        "/DFILE_VERSION=$($version.FileVersion)", "/DPRODUCT_VERSION=$($version.ProductVersion)"
        "/DFILE_NUMERIC=$fileNumeric", "/DPRODUCT_NUMERIC=$productNumeric", (Join-Path $PSScriptRoot 'ARBC.nsi')
    )
    & $MakeNsis @arguments
    if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }
    Get-Item -LiteralPath $setup
} finally {
    Pop-Location
}

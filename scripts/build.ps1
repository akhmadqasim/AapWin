<#
.SYNOPSIS
    Build driver (Debug + Release) dan aapctl (Release), lalu salin paket
    driver ke out\. Tidak menandatangani; jalankan scripts\sign-package.ps1
    dari shell elevated setelah ini.
#>
param(
    [string]$MSBuild = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
    [string]$SdkVersion = "10.0.28000.0",
    [switch]$CodeAnalysis
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$common = @("/p:Platform=x64", "/p:WindowsTargetPlatformVersion=$SdkVersion", "/nologo", "/v:m")
if ($CodeAnalysis) { $common += "/p:RunCodeAnalysis=true" }

foreach ($cfg in "Debug", "Release") {
    & $MSBuild "$root\driver\AapL2cap\AapL2cap.vcxproj" "/p:Configuration=$cfg" @common
    if ($LASTEXITCODE -ne 0) { throw "driver $cfg build failed" }
}
& $MSBuild "$root\tool\aapctl\aapctl.vcxproj" "/p:Configuration=Release" @common
if ($LASTEXITCODE -ne 0) { throw "aapctl build failed" }

$pkgs = @(
    @{ src = "$root\driver\AapL2cap\x64\Release"; dst = "$root\out\AapL2cap" },
    @{ src = "$root\driver\AapL2cap\x64\Debug";   dst = "$root\out\Debug\AapL2cap" }
)
foreach ($p in $pkgs) {
    New-Item -ItemType Directory -Force $p.dst | Out-Null
    Copy-Item "$($p.src)\AapL2cap\AapL2cap.sys", "$($p.src)\AapL2cap\AapL2cap.inf", "$($p.src)\AapL2cap.pdb" $p.dst -Force
    Write-Host "package -> $($p.dst)"
}
Write-Host "aapctl  -> $root\out\aapctl.exe"

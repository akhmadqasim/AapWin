<#
.SYNOPSIS
    Buat catalog (inf2cat) dan tanda tangani AapL2cap.sys + aapl2cap.cat
    dengan sertifikat test "CN=AapWin Test" di Cert:\LocalMachine\My.

.NOTES
    Private key sertifikat ada di machine store, jadi script ini HARUS
    dijalankan dari PowerShell yang elevated (Run as Administrator).
    Tidak menginstall apa pun; hanya inf2cat + signtool + verify.

.EXAMPLE
    pwsh -File E:\AapWin\scripts\sign-package.ps1 -PackageDir E:\AapWin\out\AapL2cap
#>
param(
    [string]$PackageDir = "E:\AapWin\out\AapL2cap",
    [string]$Thumbprint = "1AC618F9730121AFA692F49C7E5E20E16F8A5F6B",
    [string]$KitBin = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0"
)

$ErrorActionPreference = "Stop"
$inf2cat  = Join-Path $KitBin "x86\inf2cat.exe"
$signtool = Join-Path $KitBin "x64\signtool.exe"

if (-not (Test-Path (Join-Path $PackageDir "AapL2cap.inf"))) { throw "AapL2cap.inf tidak ada di $PackageDir" }
if (-not (Test-Path (Join-Path $PackageDir "AapL2cap.sys"))) { throw "AapL2cap.sys tidak ada di $PackageDir" }

Write-Host "== inf2cat $PackageDir"
& $inf2cat "/driver:$PackageDir" /os:10_X64
if ($LASTEXITCODE -ne 0) { throw "inf2cat gagal ($LASTEXITCODE)" }

Write-Host "== signtool sign"
& $signtool sign /v /sm /s My /sha1 $Thumbprint /fd sha256 `
    (Join-Path $PackageDir "AapL2cap.sys") (Join-Path $PackageDir "aapl2cap.cat")
if ($LASTEXITCODE -ne 0) { throw "signtool sign gagal ($LASTEXITCODE) - jalankan sebagai Administrator?" }

Write-Host "== signtool verify /pa"
& $signtool verify /v /pa (Join-Path $PackageDir "AapL2cap.sys") (Join-Path $PackageDir "aapl2cap.cat")
if ($LASTEXITCODE -ne 0) { throw "signtool verify gagal ($LASTEXITCODE)" }

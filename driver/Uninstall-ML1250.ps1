<#
.SYNOPSIS
    Removes the Samsung ML-1250 PCL5e driver, its queues, and the self-signed
    certificate the installer added to this machine's trust stores.
#>

[CmdletBinding()]
param(
    [switch] $KeepCertificate
)

$ErrorActionPreference = 'Continue'

$Models = @('Samsung ML-1250 PCL5e', 'Samsung ML-1250 PCL5e (with status)')
$CertSubject = 'CN=ML-1250 Driver Signing (self-signed)'

function Step([string] $m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Note([string] $m) { Write-Host "    $m" -ForegroundColor DarkGray }
function Good([string] $m) { Write-Host "  [+] $m" -ForegroundColor Green }

if (-not ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Run this from an elevated PowerShell.' -ForegroundColor Red
    exit 1
}

Step 'Removing print queues that use this driver'
foreach ($p in (Get-Printer -ErrorAction SilentlyContinue |
                Where-Object { $Models -contains $_.DriverName })) {
    Remove-Printer -Name $p.Name -ErrorAction SilentlyContinue
    Good "removed queue '$($p.Name)'"
}

Step 'Removing the printer driver'
foreach ($m in $Models) {
    if (Get-PrinterDriver -Name $m -ErrorAction SilentlyContinue) {
        Remove-PrinterDriver -Name $m -ErrorAction SilentlyContinue
        Good "removed driver '$m'"
    }
}

Step 'Removing the package from the driver store'
$removed = $false
foreach ($f in (Get-ChildItem "$env:SystemRoot\INF\oem*.inf" -ErrorAction SilentlyContinue)) {
    if ((Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue) -match 'ml1250\.gpd') {
        & pnputil.exe /delete-driver $f.Name /uninstall | Out-Null
        Good "deleted $($f.Name)"
        $removed = $true
    }
}
if (-not $removed) { Note 'no staged package found' }

if (-not $KeepCertificate) {
    Step 'Removing the self-signed certificate from the trust stores'
    foreach ($store in 'Root','TrustedPublisher','My') {
        foreach ($c in (Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
                        Where-Object { $_.Subject -eq $CertSubject })) {
            Remove-Item $c.PSPath -Force -ErrorAction SilentlyContinue
            Good "removed from $store ($($c.Thumbprint))"
        }
    }
}

Write-Host ''
Good 'Done.'
Note 'The spooler sometimes keeps a driver loaded until it is restarted.'
Note 'If the driver is still listed, run: Restart-Service -Name Spooler -Force'

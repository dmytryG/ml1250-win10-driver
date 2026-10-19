<#
.SYNOPSIS
    Installs the Samsung ML-1250 PCL5e printer driver on Windows 10/11.

.DESCRIPTION
    Windows will not stage a printer driver package that has no signed
    catalog - there is no policy level that allows unsigned print drivers and
    pnputil has no override - so this script creates a self-signed code
    signing certificate, catalogs and signs the two-file driver package with
    it, trusts that certificate on this machine, and then installs.

    Everything it uses ships with Windows: New-SelfSignedCertificate,
    New-FileCatalog, Set-AuthenticodeSignature, certutil and pnputil.  No
    WDK, no Windows SDK, no Visual Studio.

    The driver itself contains no executable code.  It is a Unidrv
    "minidriver": one text file (ml1250.gpd) describing the printer, on top of
    Microsoft's own in-box unidrv.dll.  Nothing third-party is loaded into the
    print spooler.

.PARAMETER PrinterName
    Name for the print queue.  Omit to skip creating a queue.

.PARAMETER PortName
    Port to bind the queue to.  Defaults to the USB00x port of the ML-1250
    (found by its USB VID/PID), or the only USB00x port if there is just one.
    Use LPT1: for a parallel connection.

.PARAMETER WithStatus
    Install the variant that adds Microsoft's PJL language monitor, so the
    queue can read paper-out and toner status back from the printer.  It
    needs a genuinely bidirectional port: over a USB-to-parallel adapter, or
    a one-way LPT port, status queries can stall the queue.  The default is
    the plain write-only driver, which always works.

.PARAMETER KeepCertificate
    Keep the signing certificate in the personal store afterwards.  By
    default the private key is deleted once signing is done, so the key
    cannot later be used to sign anything else.

.PARAMETER CheckOnly
    Report what the installer would find and change nothing at all.  Run this
    first on the target machine.

.EXAMPLE
    .\Install-ML1250.ps1 -CheckOnly

.EXAMPLE
    .\Install-ML1250.ps1 -PrinterName "Samsung ML-1250"

.NOTES
    Connect the printer and switch it on BEFORE running this.  The USB00x
    port only exists while Windows has the device, and it is created by the
    in-box usbprint.sys binding, independently of any driver.
#>

[CmdletBinding()]
param(
    [string] $PrinterName,
    [string] $PortName,
    [switch] $WithStatus,
    [switch] $KeepCertificate,
    [switch] $CheckOnly
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$ModelPlain = 'Samsung ML-1250 PCL5e'
$ModelBidi  = 'Samsung ML-1250 PCL5e (with status)'
$Model      = if ($WithStatus) { $ModelBidi } else { $ModelPlain }
$CertSubject = 'CN=ML-1250 Driver Signing (self-signed)'

function Fail([string] $m) { Write-Host "  [x] $m" -ForegroundColor Red; exit 1 }
function Step([string] $m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Note([string] $m) { Write-Host "    $m" -ForegroundColor DarkGray }
function Good([string] $m) { Write-Host "  [+] $m" -ForegroundColor Green }

# The USB00x port that belongs to the ML-1250, or $null.  usbprint.sys keeps
# the port number under the printer's device interface, keyed by VID/PID, and
# that survives whatever driver is or is not bound.  The port's description
# does not: it reads "SamsungML-1250" only while a driver is bound and falls
# back to a generic "Virtual printer port for USB" otherwise.
function Find-ML1250Port {
    $ifaces = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{28d78fad-5a12-11d1-ae5b-0000f803a8c2}'
    $usbPorts = @(Get-PrinterPort | Where-Object { $_.Name -like 'USB*' } |
                  ForEach-Object Name)
    $byId = Get-ChildItem $ifaces -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -match 'VID_04E8&PID_300E' } |
        ForEach-Object { Get-ItemProperty (Join-Path $_.PSPath '#\Device Parameters') -ErrorAction SilentlyContinue } |
        Where-Object { $_.'Base Name' -and $_.'Port Number' } |
        ForEach-Object { '{0}{1:000}' -f $_.'Base Name', $_.'Port Number' } |
        Where-Object { $usbPorts -contains $_ } |
        Select-Object -First 1
    if ($byId) { return $byId }
    Get-PrinterPort | Where-Object { $_.Name -like 'USB*' -and $_.Description -match 'ML-?1250' } |
        Select-Object -First 1 -ExpandProperty Name
}

# ---------------------------------------------------------------- prechecks

if (-not $CheckOnly -and
    -not ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'Run this from an elevated PowerShell (right-click, Run as administrator).'
}

Step 'Checking Windows protected print mode'
$wppBlocked = $false
foreach ($k in @('HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Printers\WPP',
                 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Print\WPP')) {
    if (Test-Path $k) {
        $v = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
        foreach ($n in 'WindowsProtectedPrintGroupPolicyState','WindowsProtectedPrintMode','EnabledBy') {
            if ($null -ne $v.$n -and $v.$n -ne 0) { $wppBlocked = $true }
        }
    }
}
if ($wppBlocked) {
    Write-Host ''
    Write-Host 'Windows protected print mode is enabled on this machine.' -ForegroundColor Yellow
    Write-Host 'While it is on, Windows refuses to install any third-party printer driver'
    Write-Host 'and deletes ones that are already there.  Only IPP printers work.'
    Write-Host ''
    Write-Host 'Turn it off first:  Settings > Bluetooth & devices > Printers & scanners'
    Write-Host '                    > Printer preferences > Windows protected print mode > Off'
    Write-Host 'If it was set by group policy your administrator has to do it.'
    Fail 'Refusing to half-install. Nothing was changed.'
}
Good 'not enabled'

foreach ($f in 'ml1250.inf','ml1250.gpd') {
    if (-not (Test-Path (Join-Path $here $f))) { Fail "missing file: $f" }
}
Good 'driver files present'

# ----------------------------------------------------------- report only

if ($CheckOnly) {
    Step 'Windows and print stack'
    Note ("Windows       : {0} build {1}" -f
          (Get-CimInstance Win32_OperatingSystem).Caption,
          [Environment]::OSVersion.Version.Build)
    Note ("Architecture  : {0}" -f $env:PROCESSOR_ARCHITECTURE)
    $core = "$env:SystemRoot\System32\spool\drivers\x64\3"
    foreach ($f in 'UNIDRV.DLL','UNIDRVUI.DLL','UNIRES.DLL','STDNAMES.GPD','PJLMON.DLL') {
        $p = Join-Path $core $f
        Note ("{0,-14}: {1}" -f $f, $(if (Test-Path $p) { 'present' } else { 'MISSING' }))
    }
    if (-not (Test-Path (Join-Path $core 'UNIDRV.DLL'))) {
        Note 'Unidrv is not staged yet. That is normal on a machine with no v3'
        Note 'printer installed; the INF pulls it from the in-box ntprint.inf.'
    }

    Step 'Ports'
    $ports = Get-PrinterPort | Where-Object { $_.Name -like 'USB*' -or $_.Name -like 'LPT*' }
    if ($ports) { $ports | ForEach-Object { Note ("{0,-12} {1}" -f $_.Name, $_.Description) } }
    else        { Note 'no USB00x or LPTn port found' }
    if (-not ($ports | Where-Object Name -like 'USB*')) {
        Note 'No USB00x port. Connect the printer and switch it on, then re-check.'
        Note 'Windows publishes that port only while it has the device.'
    }
    $mine = Find-ML1250Port
    Note ("ML-1250 port : {0}" -f $(if ($mine) { $mine } else { 'not found - pass -PortName' }))

    Step 'Existing printer drivers'
    Get-PrinterDriver | Where-Object { $_.Name -like '*ML-1250*' -or $_.Name -like '*Samsung*' } |
        ForEach-Object { Note ("{0} (v{1})" -f $_.Name, $_.MajorVersion) }

    Write-Host ''
    Good 'Check complete. Nothing was changed.'
    exit 0
}

# --------------------------------------------------------- sign the package

$cat = Join-Path $here 'ml1250.cat'
Step 'Creating a signing certificate and signing the driver package'
Note 'This adds a self-signed certificate to this machine''s trusted root and'
Note 'trusted publisher stores.  Anything signed with that key becomes trusted'
Note 'machine-wide.  Uninstall-ML1250.ps1 removes it again.'

$cert = New-SelfSignedCertificate `
            -Subject $CertSubject `
            -Type CodeSigningCert `
            -KeyAlgorithm RSA -KeyLength 3072 `
            -HashAlgorithm SHA256 `
            -CertStoreLocation 'Cert:\LocalMachine\My' `
            -NotAfter (Get-Date).AddYears(20)
Good ("certificate {0}" -f $cert.Thumbprint)

# The catalog must list every file the INF installs, hashed.  CatalogVersion 2
# is SHA-256; version 1 is SHA-1 and is rejected on modern Windows.
if (Test-Path $cat) { Remove-Item $cat -Force }
New-FileCatalog -CatalogFilePath $cat -CatalogVersion 2 -Path @(
    (Join-Path $here 'ml1250.inf'),
    (Join-Path $here 'ml1250.gpd')
) | Out-Null
Good 'ml1250.cat built'

$sig = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert `
                                 -HashAlgorithm SHA256
if ($sig.Status -ne 'Valid' -and $sig.Status -ne 'UnknownError') {
    Note ("signature status: {0} - {1}" -f $sig.Status, $sig.StatusMessage)
}

$cer = Join-Path $env:TEMP 'ml1250-signing.cer'
Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null
foreach ($store in 'Root','TrustedPublisher') {
    # Root makes the chain verify; TrustedPublisher is what stops Windows
    # putting up a "do you want to install this device software?" prompt,
    # which a scripted install cannot answer.
    & certutil.exe -f -addstore $store $cer | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "certutil could not write to the $store store" }
}
Good 'certificate trusted (Root + TrustedPublisher)'

if (-not $KeepCertificate) {
    Remove-Item ("Cert:\LocalMachine\My\{0}" -f $cert.Thumbprint) -Force -ErrorAction SilentlyContinue
    Note 'signing key discarded; only the public certificate remains trusted'
}
Remove-Item $cer -Force -ErrorAction SilentlyContinue

# -------------------------------------------------------- stage and install

Step 'Staging the driver package into the Windows driver store'
$inf = Join-Path $here 'ml1250.inf'
$out = & pnputil.exe /add-driver "$inf" /install 2>&1
$out | ForEach-Object { Note $_ }
if ($LASTEXITCODE -ne 0) {
    Fail ("pnputil failed with exit code {0}. 0xE000022F means the catalog was " +
          "not accepted; 0xE0000243 means the publisher is not trusted." -f $LASTEXITCODE)
}

# Add-PrinterDriver needs the published name in the driver store, not our copy.
$published = ($out | Select-String -Pattern 'oem\d+\.inf' -AllMatches |
              ForEach-Object { $_.Matches.Value } | Select-Object -First 1)
if (-not $published) {
    $published = (Get-ChildItem "$env:SystemRoot\INF\oem*.inf" |
                  Sort-Object LastWriteTime -Descending |
                  Where-Object { (Get-Content $_.FullName -Raw) -match 'ml1250\.gpd' } |
                  Select-Object -First 1).Name
}
if (-not $published) { Fail 'the package staged but I cannot find its oemNN.inf' }
Good "staged as $published"

Step "Registering the driver: $Model"
Add-PrinterDriver -Name $Model -InfPath (Join-Path "$env:SystemRoot\INF" $published)
Good 'driver registered'

# ------------------------------------------------------------- the queue

if (-not $PrinterName) {
    Write-Host ''
    Good 'Driver installed.'
    Note 'Add the printer yourself, or re-run with -PrinterName "Samsung ML-1250".'
    exit 0
}

if (-not $PortName) {
    # Taking the first USB port blindly picks whatever other printer was
    # plugged in first.
    $usbPorts = @(Get-PrinterPort | Where-Object { $_.Name -like 'USB*' })
    $usb = Find-ML1250Port
    if (-not $usb -and $usbPorts.Count -eq 1) { $usb = $usbPorts[0].Name }
    if (-not $usb -and $usbPorts.Count -gt 1) {
        Write-Host ''
        Write-Host 'Several USB printer ports and none of them belongs to an ML-1250:' -ForegroundColor Yellow
        $usbPorts | ForEach-Object { Write-Host ("    {0,-8} {1}" -f $_.Name, $_.Description) }
        Write-Host 'Re-run with -PortName USB00x.'
        Note 'The driver itself is installed; only the queue is missing.'
        exit 2
    }
    if ($usb) {
        $PortName = $usb
    } else {
        Write-Host ''
        Write-Host 'No USB00x port found.' -ForegroundColor Yellow
        Write-Host 'That port is published by Windows only while the printer is connected'
        Write-Host 'and switched on.  Plug it in, power it up, and re-run - or pass'
        Write-Host '-PortName LPT1: for a parallel cable.'
        Write-Host ''
        Note 'The driver itself is installed; only the queue is missing.'
        exit 2
    }
}
Step "Creating the queue on $PortName"
if (Get-Printer -Name $PrinterName -ErrorAction SilentlyContinue) {
    Note 'a printer with that name already exists; leaving it alone'
} else {
    Add-Printer -Name $PrinterName -DriverName $Model -PortName $PortName
    Good "printer '$PrinterName' created on $PortName"
}

Write-Host ''
Good 'Done.'
Note 'Print a Windows test page first.  If it comes out garbled or blank, try'
Note 'the 300 dpi setting in Printing Preferences - this printer has 4 MB of'
Note 'RAM and a complex 600 dpi page can exhaust it.'

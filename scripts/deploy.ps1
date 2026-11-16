# Installs (or removes) the canon-eos-webcam video source. Must run as administrator.
#
#   deploy.ps1              copies build\Release to "C:\Program Files\CanonEosWebcam" and registers the DLL
#   deploy.ps1 -Uninstall   unregisters the DLL and deletes the files
#
# After installing, run (without admin):  canon-eos-webcam install   and then   canon-eos-webcam run
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\Release'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$dest = Join-Path $env:ProgramFiles 'CanonEosWebcam'
$data = 'C:\ProgramData\CanonEosWebcam'
New-Item -ItemType Directory -Force $data | Out-Null
Start-Transcript -Path (Join-Path $data 'deploy.log') -Force | Out-Null

try {
    # The Frame Server keeps the DLL loaded; stopping it frees the file (it restarts on demand).
    foreach ($svc in 'FrameServerMonitor', 'FrameServer') {
        Stop-Service $svc -Force -ErrorAction SilentlyContinue
    }

    $dll = Join-Path $dest 'canon-eos-webcam-source.dll'
    if ($Uninstall) {
        if (Test-Path $dll) { & regsvr32.exe /s /u $dll; Write-Host "DLL unregistered." }
        Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Files removed from $dest."
        return
    }

    # Upgrade from the old name (eoscam): stop the old bridge and remove its folder. Its DLL had the same
    # CLSID, so registering the new DLL below takes over the registration.
    Get-Process eoscam, eoscam-bg -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $env:ProgramFiles 'EosCam') -Recurse -Force -ErrorAction SilentlyContinue

    New-Item -ItemType Directory -Force $dest | Out-Null
    # Removes leftovers from previous updates; if they are still in use, they wait for the next one.
    Get-ChildItem $dest -Filter '*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    foreach ($name in 'canon-eos-webcam-source.dll', 'canon-eos-webcam.exe', 'canon-eos-webcam-bg.exe') {
        $src = Join-Path $BuildDir $name
        if (-not (Test-Path $src)) { throw "$src not found. Build first (cmake --build build --config Release)." }
        $target = Join-Path $dest $name
        if (Test-Path $target) {
            # A loaded file can't be overwritten, but it can be renamed.
            try { Remove-Item $target -Force }
            catch { Rename-Item $target "$name.old-$(Get-Date -Format yyyyMMddHHmmss)" -Force }
        }
        Copy-Item $src $target -Force
    }

    $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed (code $($p.ExitCode))." }

    # The DLL runs as LocalService and writes its log to ProgramData\CanonEosWebcam.
    & icacls.exe $data /grant '*S-1-5-19:(OI)(CI)M' /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null

    Write-Host "Installed in $dest and DLL registered."
}
catch {
    Write-Host "ERROR: $_"
    exit 1
}
finally {
    Stop-Transcript | Out-Null
}

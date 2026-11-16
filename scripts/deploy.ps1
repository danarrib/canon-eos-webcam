# Instala (ou remove) a fonte de vídeo do eoscam. Precisa rodar como administrador.
#
#   deploy.ps1              copia build\Release para "C:\Program Files\EosCam" e registra a DLL
#   deploy.ps1 -Uninstall   desregistra a DLL e apaga os arquivos
#
# Depois de instalar, rode (sem admin):  eoscam install   e então   eoscam run
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\Release'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$dest = Join-Path $env:ProgramFiles 'EosCam'
$data = 'C:\ProgramData\EosCam'
New-Item -ItemType Directory -Force $data | Out-Null
Start-Transcript -Path (Join-Path $data 'deploy.log') -Force | Out-Null

try {
    # O Frame Server mantém a DLL carregada; pará-lo libera o arquivo (ele reinicia sob demanda).
    foreach ($svc in 'FrameServerMonitor', 'FrameServer') {
        Stop-Service $svc -Force -ErrorAction SilentlyContinue
    }

    $dll = Join-Path $dest 'eoscam-source.dll'
    if ($Uninstall) {
        if (Test-Path $dll) { & regsvr32.exe /s /u $dll; Write-Host "DLL desregistrada." }
        Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Arquivos removidos de $dest."
        return
    }

    New-Item -ItemType Directory -Force $dest | Out-Null
    # Remove sobras de atualizações anteriores; se ainda estiverem em uso, ficam para a próxima.
    Get-ChildItem $dest -Filter '*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    foreach ($name in 'eoscam-source.dll', 'eoscam.exe', 'eoscam-bg.exe') {
        $src = Join-Path $BuildDir $name
        if (-not (Test-Path $src)) { throw "Não encontrei $src. Compile antes (cmake --build build --config Release)." }
        $target = Join-Path $dest $name
        if (Test-Path $target) {
            # Um arquivo carregado não pode ser sobrescrito, mas pode ser renomeado.
            try { Remove-Item $target -Force }
            catch { Rename-Item $target "$name.old-$(Get-Date -Format yyyyMMddHHmmss)" -Force }
        }
        Copy-Item $src $target -Force
    }

    $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 falhou (código $($p.ExitCode))." }

    # A DLL roda como LocalService e grava o log em ProgramData\EosCam.
    & icacls.exe $data /grant '*S-1-5-19:(OI)(CI)M' /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null

    Write-Host "Instalado em $dest e DLL registrada."
}
catch {
    Write-Host "ERRO: $_"
    exit 1
}
finally {
    Stop-Transcript | Out-Null
}

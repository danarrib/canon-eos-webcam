# eoscam: Canon EOS como webcam nativa do Windows 11

Usa uma Canon EOS antiga (testado com a **EOS Rebel T3 / 1100D**) como webcam, sem os
drivers da Canon: só o driver PTP/WPD padrão do Windows e a API de câmera virtual do
Windows 11 (`MFCreateVirtualCamera`).

| | EOS Webcam Utility | eoscam |
|---|---|---|
| Resolução | ~480×360 | 1280×720 (recorte 16:9 do Live View de 1056×704) |
| Quadros/s | — | ~28 (o Live View da T3 entrega ~29) |
| CPU (sistema todo, 32 threads) | alta | ~3% |
| Live View | sempre ligado | liga só quando algum programa usa a webcam |

## Como funciona

```
Canon T3 ──USB/PTP (driver WPD do Windows)──> eoscam-bg.exe / eoscam run      (sessão do usuário)
                                                  │ JPEG 1056×704
                                                  ▼
                                memória compartilhada Global\EosCamFrames1
                                                  │
                                                  ▼
                         eoscam-source.dll  (carregada pelo Frame Server, LocalService)
                         decodifica JPEG (WIC) → recorta/escala → NV12 1280×720 etc.
                                                  │
                                                  ▼
                 câmera virtual "Canon EOS Webcam (eoscam)"  →  Teams, Zoom, navegador, OBS...
```

- **`eoscam-source.dll`**: fonte de mídia (IMFMediaSource) registrada em HKLM. O Frame
  Server a carrega quando um programa abre a webcam. Ela sinaliza no canal compartilhado
  que quer quadros; sem quadros, mostra uma tela explicando o que falta.
- **`eoscam.exe` / `eoscam-bg.exe`**: a ponte com a câmera. Liga o Live View
  (`SetRemoteMode`, `EVFOutputDevice = PC`) quando a fonte pede quadros, busca os quadros com
  `GetViewFinderData` e desliga o Live View 5 s depois que ninguém mais usa a webcam. Se outro
  software derrubar o Live View, religa automaticamente.
- **`tools/eosprobe`**: ferramenta de medição do Live View via PTP.

Formato oferecido: só NV12 1280×720 a 30 fps, recorte 16:9 ancorado na base (descarta o topo).
Com várias opções, alguns programas abriam primeiro em 640×480 (4:3) e trocavam para 16:9 um
segundo depois.

## Compilar

Requer Visual Studio 2022 (C++), CMake e o Windows SDK 10.0.22000+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Instalar

```powershell
# 1. Como administrador: copia para C:\Program Files\EosCam e registra a DLL
pwsh -ExecutionPolicy Bypass -File scripts\deploy.ps1

# 2. Como usuário comum:
& "C:\Program Files\EosCam\eoscam.exe" install          # cria a câmera virtual
& "C:\Program Files\EosCam\eoscam.exe" autostart on     # ponte em segundo plano, inicia com o Windows
```

A câmera deve estar ligada, num modo de foto (P/Av/Tv/M) e com o Live View habilitado no menu.

## Comandos

```
eoscam run                  ponte neste console (para ver o log ao vivo)
eoscam start | stop         ponte em segundo plano (eoscam-bg.exe)
eoscam autostart on|off     iniciar a ponte junto com o Windows
eoscam install | uninstall  criar/remover a câmera virtual
eoscam status               estado do canal (quadros, produtor, consumidor)
eoscam list                 câmeras que o Windows enxerga
eoscam test [--seconds N] [--size LxA] [--out f.jpg]
                            abre a webcam como um app comum e mede quadros/s e CPU
```

Logs: `C:\ProgramData\EosCam\eoscam.log` (ponte) e `source.log` (fonte, dentro do Frame Server).

## Desenvolvimento

`eoscam test --direct` carrega a DLL dentro do próprio processo, sem registro nem Frame
Server. Com `EOSCAM_LOCAL=1` no ambiente (no `run` e no `test`), o canal usa o namespace
`Local\`, então dá para testar tudo sem administrador:

```powershell
$env:EOSCAM_LOCAL = '1'
.\build\Release\eoscam.exe run        # em um terminal
.\build\Release\eoscam.exe test --direct   # em outro
```

Para atualizar a DLL instalada, rode `scripts\deploy.ps1` de novo (como admin): ele para o
Frame Server para liberar o arquivo.

## Desinstalar

```powershell
& "C:\Program Files\EosCam\eoscam.exe" autostart off
& "C:\Program Files\EosCam\eoscam.exe" stop
& "C:\Program Files\EosCam\eoscam.exe" uninstall
pwsh -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Uninstall   # como admin
```

## Conflito com o EOS Webcam Utility

O driver do EOS Webcam Utility (UMDF, usa o EDSDK) fala com a câmera sempre que o Frame
Server do Windows entra em atividade e derruba o Live View do eoscam. O eoscam se recupera
sozinho em menos de 1 s, mas o ideal é desativar o dispositivo "EOS Webcam Utility" no
Gerenciador de Dispositivos ou desinstalar o EOS Webcam Utility.

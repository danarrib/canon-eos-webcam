# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

O eoscam transforma uma Canon EOS (testada: Rebel T3 / 1100D) em webcam nativa do Windows 11 usando só o driver
PTP/WPD padrão e o `MFCreateVirtualCamera`, com efeitos opcionais do NVIDIA Maxine na GPU. Só Windows, C++20 (MSVC).
O README.md é a referência de uso (comandos, variáveis de ambiente, efeitos, desempenho medido);
`docs/especificacao-v2-gpu-painel.md` é a especificação da v2.

**Idioma:** comentários, mensagens de log, textos da interface, README e mensagens de commit são em português do
Brasil. Mantenha os novos em português.

## Compilar

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --config Release --target eoscam   # um alvo só
```

Alvos: `eoscam-source` (DLL), `eoscam` (app de console), `eoscam-bg` (mesmo código, subsistema `WIN32`, com
`EOSCAM_BACKGROUND` definido → ícone na bandeja + painel), `maxine-proxy` (biblioteca estática), `eosprobe`
(`tools/eosprobe`). O CRT é estático de propósito: a DLL roda dentro do Frame Server e não pode depender do VC++
Redistributable. Compila com `/W4 /permissive-`; mantenha a compilação sem avisos.

## Testar

Não há framework de testes unitários. A verificação é feita pelos próprios subcomandos do app:

```powershell
$env:EOSCAM_LOCAL = '1'                       # canal em Local\ → sem admin e sem Frame Server
.\build\Release\eoscam.exe run                # ponte no console (antes pare a instalada: eoscam stop)
.\build\Release\eoscam.exe test --direct      # carrega a DLL no próprio processo, mede quadros/s e CPU
.\build\Release\eoscam.exe process in.jpg --bench 100   # pipeline sobre um JPEG, sem câmera
.\build\Release\eoscam.exe cam                # diagnóstico dos ajustes da câmera (com a ponte parada)
```

Variáveis de ambiente úteis nos testes: `EOSCAM_SETTINGS` (outro settings.ini), `EOSCAM_LOG`,
`NV_VIDEO_EFFECTS_PATH` (uma pasta vazia simula a ausência do runtime do Maxine), `EOSCAM_FORCE_CPU_BLUR`,
`EOSCAM_THREADS`, `EOSCAM_DUMP_EVENTS`, `EOSCAM_CUDA_SPIN`, `EOSCAM_PANEL_NOACTIVATE=1` (o painel aparece sem
roubar o foco).

**Regra para testar a interface:** o usuário trabalha nesta mesma máquina. Nunca use entrada sintética (SendInput,
SendKeys, cliques/digitação via UI Automation), nunca roube o foco e nunca abra diálogos modais. Teste o painel
com `EOSCAM_PANEL_NOACTIVATE=1`, mensagens de janela para os nossos próprios HWNDs, capturas com `PrintWindow` e
arquivos de configuração. Repita esta regra no prompt de qualquer subagente que mexa na interface.

Logs: `C:\ProgramData\EosCam\eoscam.log` (ponte, estatísticas a cada 60 s) e `source.log` (DLL).

## Instalar (deploy)

`scripts/deploy.ps1` precisa rodar como administrador (copia `build\Release` para `C:\Program Files\EosCam`, para o
Frame Server para liberar a DLL, renomeia arquivos em uso para `*.old-*` e roda o `regsvr32`). Este shell não é
elevado: rode com `Start-Process pwsh -Verb RunAs -ArgumentList '-ExecutionPolicy','Bypass','-File','scripts\deploy.ps1'`,
avise o usuário que vai aparecer um prompt do UAC e agrupe as mudanças na DLL para pedir elevação poucas vezes.
Depois, sem elevação: `eoscam install` (cria a câmera virtual) e `eoscam start` / `autostart on`.

## Arquitetura

Dois processos em sessões diferentes, ligados por memória compartilhada:

- **`src/source/` → `eoscam-source.dll`**: um `IMFMediaSource` registrado em HKLM (CLSID em `shared.h`),
  carregado pelo Frame Server do Windows como LocalService na sessão 0. Enxuta de propósito: sinaliza que quer
  quadros, copia o NV12 pronto mais recente e desenha as telas de aviso (`renderer.cpp`, GDI) quando falta
  câmera, ponte ou as versões não batem. **Nunca acrescente WIC, Maxine, CUDA ou dependências pesadas aqui.**
- **`src/app/` → `eoscam.exe` / `eoscam-bg.exe`**: a ponte, na sessão do usuário.
  - `canon_camera.*`: PTP via WPD (operações de fabricante da Canon: `SetRemoteMode`, `EVFOutputDevice`,
    `GetViewFinderData`, `GetEvent`, escrita de propriedades, incluindo o Picture Style `0xD150…` decifrado na
    própria câmera).
  - `bridge.*`: duas threads de trabalho. A de captura é a **única dona da sessão PTP** (conecta, busca os JPEGs,
    lê os eventos e executa entre um quadro e outro os comandos enfileirados pelo painel). A de processamento
    pega sempre o JPEG mais recente e descarta os outros, para a captura seguir a ~29 fps mesmo com o
    processamento lento. O Live View só liga com um consumidor ativo ou o painel aberto, desliga 5 s depois, e é
    religado sozinho se algo o derrubar.
  - `pipeline.*`: JPEG → decodificação WIC → recorte 16:9 ancorado na base (1056×594) → Denoise / Artifact
    Reduction / máscara do Green Screen / Super Resolution 2× opcionais do Maxine (`maxine.*`) → escala na CPU +
    ajustes por LUT + composição do fundo + NV12 1920×1080 (`imaging.*`, AVX2 com alternativa em C++ puro).
    Trocar o modo de um efeito recria só esse efeito, "a quente", sem parar os quadros. Sem o runtime do Maxine
    ou com falha na GPU, os efeitos ficam `Unavailable`/`Failed` e o pipeline segue só na CPU.
  - `settings.*`: `SettingsStore` com contador de versão; quem consome compara a versão de `Get(&version)` para
    detectar mudanças, uma thread grava `%APPDATA%\EosCam\settings.ini` com atraso (debounce) e edições externas
    do arquivo são recarregadas.
  - `panel.*`: ícone na bandeja + painel de controle Win32 (só no `eoscam-bg`). `main.cpp` despacha os subcomandos.
- **`src/common/shared.h`**: o canal de quadros. Mapeamento `Global\EosCamFrames2` com um anel de 3 slots
  (seqlock) de quadros NV12 1920×1080 prontos, mais os sinais de vida de produtor/consumidor e o
  `ProducerState`. **A DLL cria** o mapeamento (usuários não têm `SeCreateGlobalPrivilege`); o app só abre e
  mantém o handle aberto. Os nomes terminam em `2` para uma v1 instalada nunca se misturar com a v2. Mudar o
  layout exige subir `kLayoutVersion` e tratar DLL instalada + ponte como um par casado (senão a DLL mostra
  "versões incompatíveis").
- **`third_party/maxine`**: cabeçalhos e proxies (MIT) que carregam `NVVideoEffects.dll` / `NVCVImage.dll`
  dinamicamente do runtime instalado. Nada é linkado contra o CUDA Toolkit; mantenha assim.

## Restrições

- Mantenha o EOS Webcam Utility / EDSDK da Canon fora do caminho; o driver UMDF dele derruba o nosso Live View
  quando está ativo (o usuário deixa esse dispositivo desativado). A ideia do projeto é não depender de drivers
  incomuns.
- A câmera virtual oferece um único formato (NV12 1920×1080 a 30 fps); com vários, alguns programas abriam
  primeiro em 4:3 e trocavam depois.
- Os valores das propriedades da câmera ficam na própria câmera, não no settings.ini; os valores disponíveis vêm
  dos eventos da câmera, e só ficam habilitadas as propriedades que o modo atual anuncia.

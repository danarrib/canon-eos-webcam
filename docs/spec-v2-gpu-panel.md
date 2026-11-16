# eoscam v2: processamento na GPU (NVIDIA Maxine) e painel de controle

Status: especificação aprovada pelo usuário em 2026-11-16. Leia junto com o `README.md`.

## Objetivo

1. Mover o processamento de imagem para a ponte (`eoscam-bg.exe` / `eoscam run`), na GPU,
   com os efeitos do NVIDIA Maxine Video Effects, e entregar **1920×1080** à câmera virtual.
2. Criar um **painel de controle** (ícone na bandeja + janela) com ajustes de imagem, efeitos
   de IA, fundo virtual e ajustes da própria câmera, aplicados ao vivo e persistidos.

## Estado atual (v1)

- `src/app/canon_camera.*`: PTP pelo WPD (`PtpDevice`, `CanonCamera`). Live View via
  `GetViewFinderData` (JPEG 1056×704, 4:2:2, ~29 fps). Religa o Live View sozinho se ele parar.
- `src/app/main.cpp`: comandos `run/start/stop/autostart/install/uninstall/status/list/test`.
  `CmdRun` é o laço da ponte: abre o canal, liga a câmera quando `ConsumerActive()`, publica
  os JPEGs. `eoscam-bg.exe` é o mesmo código com `EOSCAM_BACKGROUND` (WIN32, log em arquivo).
- `src/common/shared.h`: canal `Global\EosCamFrames1` (a DLL cria; o app abre), 3 slots
  com seqlock, heartbeat do consumidor e do produtor. `EOSCAM_LOCAL=1` usa `Local\`.
- `src/source/*`: `eoscam-source.dll` (Frame Server, LocalService, sessão 0). Decodifica o
  JPEG com WIC, recorta 16:9 **ancorado na base**, escala para 1280×720 e converte para NV12.
  Mostra telas de aviso (`FrameRenderer::RenderMessage`) quando não há quadros.

## Arquitetura v2

```
Câmera ──PTP──> ponte (sessão do usuário)
                 ├─ thread de captura/processamento (laço atual de CmdRun, ampliado)
                 │    JPEG → decodifica (WIC, CPU) → recorte 16:9 pela base (1056×594)
                 │    → [Denoise] → [Artifact Reduction]            (GPU, Maxine, na resolução de origem)
                 │    → [Green Screen: máscara]                      (GPU, Maxine, na resolução de origem)
                 │    → Super Resolution 2× (2112×1188) ou aumento simples
                 │    → reduz para 1920×1080
                 │    → ajustes de imagem (brilho/contraste/gamma/temperatura/tom/saturação)
                 │    → composição do fundo (cor sólida / desfoque / imagem) usando a máscara
                 │    → NV12 1920×1080 → canal compartilhado
                 ├─ thread de UI (bandeja + painel), só no eoscam-bg
                 └─ fila de comandos PTP para ajustes da câmera (executada pela thread de captura,
                    que é a única dona da sessão PTP)
DLL (Frame Server): copia o NV12 pronto para a amostra. Só decodifica/renderiza as telas de aviso.
```

### Canal compartilhado (layout v2)

- Novos nomes: `Global\EosCamFrames2` / `Global\EosCamFrameReady2` (e `Local\…2`). Assim a
  v1 instalada e a v2 nunca se confundem. `kLayoutVersion = 2`.
- Os slots passam a conter **NV12 1920×1080** (3.110.400 bytes). Coloque no cabeçalho do
  slot: `format` (fourcc NV12), `width`, `height`, `stride`. Tamanho máximo do slot: 4 MB.
- O resto do protocolo (seqlock, heartbeats, estados do produtor) não muda.

### DLL

- Oferece **apenas NV12 1920×1080 @ 30 fps** (`kSizes`). Mantenha o comentário explicando
  por que há um único formato (programas abriam em 4:3 e trocavam depois).
- `RenderJpeg` deixa de ser usado no caminho normal. A DLL copia o NV12 do slot (validando
  o tamanho). Mantenha `RenderMessage` para as telas de aviso, agora em 1080p.
- Se o slot vier num tamanho diferente do formato negociado, mostre uma tela de aviso
  ("versões incompatíveis do eoscam: reinstale"), nunca quebre.

### Pipeline na ponte

Crie um módulo novo, por exemplo `src/app/pipeline.{h,cpp}` (classe `FramePipeline`),
mais `src/app/maxine.{h,cpp}` (carregamento e encapsulamento dos efeitos).

**Maxine Video Effects SDK**
- O runtime está instalado em `C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects`
  (versão 0.7.6, `NVVideoEffects.dll` 0.8.1). Os modelos (em `models\`) existem para a GPU
  do usuário (sm 89 = RTX 4080): `SR_2x/3x/4x_{con,agg}`, `AR-{con,agg}`,
  `Denoise-{con,agg}` e `AIGS_288x512_*`.
- Os headers e os "proxies" (que carregam a DLL dinamicamente) estão no repositório público
  **NVIDIA/MAXINE-VFX-SDK** no GitHub (licença MIT): `nvvfx/include/` (`nvVideoEffects.h`,
  `nvCVImage.h`, `nvCVStatus.h`, …) e `nvvfx/src/` (`nvVideoEffectsProxy.cpp`,
  `nvCVImageProxy.cpp`). Use a tag/versão mais próxima de 0.7.6. Copie só os arquivos
  necessários para `third_party/maxine/` (com o LICENSE) e versione-os.
- Localize o runtime pela variável `NV_VIDEO_EFFECTS_PATH` ou, se ela não existir, pelo
  caminho padrão acima. Defina `NVVFX_MODEL_DIRECTORY` com a pasta `models`.
- **Não dependa do CUDA Toolkit nem do nvcc.** Use só a API do SDK: `NvCVImage_Alloc`,
  `NvCVImage_Transfer` (CPU↔GPU e conversões de formato, u8↔f32, chunky↔planar),
  `NvVFX_CudaStreamCreate` e os efeitos. O que o SDK não fizer, faça na CPU.
- Efeitos e modos (confirme a semântica de `NVVFX_MODE` nos headers e na documentação):
  - Denoise: off / con / agg. É temporal, então mantenha o objeto de estado
    (`NVVFX_STATE`) e o reinicie quando a resolução ou o modo mudar.
  - Artifact Reduction: off / con / agg.
  - Super Resolution 2×: off / con / agg. Com off, o aumento é bilinear/Fant comum.
  - Green Screen (AIGS): gera a máscara na resolução de origem (antes da Super Resolution).
    A máscara é ampliada para 1080p na composição.
- Ao trocar um modo, recrie só o efeito afetado, numa troca "quente": o quadro atual usa o
  efeito antigo até o novo estar carregado (`NvVFX_Load` pode levar segundos). Quando a webcam
  é aberta, pré-carregue os efeitos ativos.
- Se o runtime não existir ou a inicialização falhar, todo efeito de IA fica indisponível
  (informe ao painel) e o pipeline segue na CPU: decodifica → recorta → escala para 1080p →
  ajustes → NV12. O Green Screen fica desabilitado nesse caso.

**Ajustes de imagem** (sobre a imagem da câmera, antes da composição com o fundo)
- Brilho (−100…+100), Contraste (−100…+100), Gamma (0,5…2,0, padrão 1,0),
  Temperatura (−100…+100, quente/frio: ganhos R/B), Tom (−100…+100, verde/magenta: ganho G),
  Saturação (0…200%, padrão 100%).
- Implementação sugerida: uma LUT de 256 entradas por canal, recalculada só quando os valores
  mudam (brilho, contraste, gamma, temperatura e tom), mais a saturação aplicada na conversão
  para NV12 (escala de U/V em torno de 128) ou numa matriz RGB. Paralelize em faixas com
  `std::thread` ou um pool simples, se for preciso para caber no orçamento.

**Fundo virtual** (só com o Green Screen ligado)
- Modos: **cor sólida** (escolhida com `ChooseColor`), **desfoque** (intensidade 0…100; use o
  efeito `BgBlur` do SDK se estiver disponível no runtime, senão um desfoque separável na CPU
  em resolução reduzida) e **imagem do disco** (PNG/JPG via WIC; preencha 1920×1080 com recorte
  central, "cover"; recarregue só quando o caminho muda).
- Composição alfa com a máscara ampliada e suavizada (evite bordas serrilhadas).

**Orçamento de desempenho** (meça e registre no log a cada 60 s)
- 1920×1080 a ≥ 25 fps com Super Resolution + Artifact Reduction ligados.
- Tempo de GPU e de CPU por estágio, por média móvel (para o painel e o log).
- Sem efeitos de IA: CPU da ponte < 25% de um núcleo.

### Ajustes da câmera via PTP

Use os códigos e codificações do libgphoto2 (`camlibs/ptp2/ptp.h`, `ptp.c` e `config.c`)
**como referência**; ele é LGPL, então não copie código, só valores e formatos.

- Valores atuais e listas de valores permitidos: interprete os registros de `GetEvent`
  (0x9116): `PropValueChanged` (0xC189) e `AvailListChanged` (0xC18A). Mantenha um cache
  `prop → valor atual + valores permitidos` e alimente o painel a partir dele.
- Escreva com `SetDevicePropValueEx` (0x9110), já usado em `CanonCamera::SetPropEx`.
- Propriedades no painel (só as que a câmera anunciar; as demais aparecem desabilitadas):
  - Compensação de exposição (`ExpCompensation`, 0xD104; codificação em terços de EV)
  - ISO (0xD103)
  - Balanço de branco (0xD109): automático, luz do dia, sombra, nublado, tungstênio,
    fluorescente, flash, personalizado; e temperatura de cor em Kelvin (verifique o código no
    libgphoto2) quando o modo for Kelvin
  - Picture Style (0xD110): Padrão, Retrato, Paisagem, Neutro, Fiel, Monocromático
  - Parâmetros do Picture Style ativo (contraste, saturação, tom de cor, nitidez), se o
    modelo permitir (verifique como o libgphoto2 trata as propriedades `PictureStyle*`)
  - Auto Lighting Optimizer, se anunciado
- Os comandos do painel entram numa fila thread-safe, executada pela thread de captura entre
  um quadro e outro. Falhas aparecem no painel ("a câmera recusou: modo automático?").
- **Os ajustes da câmera não são persistidos pelo eoscam**: a própria câmera os guarda.

### Configurações

- Arquivo `%APPDATA%\EosCam\settings.ini` (`GetPrivateProfileString`/`WritePrivateProfileString`),
  com as seções `[image]`, `[effects]`, `[background]` e `[ui]`.
- Um struct `Settings` com contador de versão, protegido por mutex, que o pipeline consulta
  uma vez por quadro. Ao mudar, grave em disco com debounce (~500 ms).

### Painel (só no `eoscam-bg.exe`)

- **Ícone na bandeja** (`Shell_NotifyIcon`). Menu: "Abrir painel", "Iniciar com o Windows"
  (marcado conforme `HKCU\…\Run\EosCam`), "Sair". Clique duplo abre o painel.
- **Janela** em Win32 nativo (sem dependências novas), com uma prévia ao vivo à esquerda
  (~640×360, ~15 fps, a partir do último quadro processado em memória, convertido para BGRA e
  desenhado com `StretchDIBits`) e controles à direita, em grupos:
  - **Imagem:** controles deslizantes de brilho, contraste, gamma, temperatura, tom e
    saturação, cada um com o valor ao lado e duplo clique para zerar; botão "Restaurar padrões".
  - **Efeitos de IA:** Super Resolution (off/con/agg), Artifact Reduction (off/con/agg),
    Denoise (off/con/agg). Desabilitados, com o motivo, se o Maxine não estiver disponível.
  - **Fundo:** Green Screen on/off; modo (cor sólida / desfoque / imagem); botão "Cor…",
    intensidade do desfoque, botão "Imagem…" (`IFileOpenDialog`, PNG/JPG) com o nome do arquivo.
  - **Câmera:** compensação de exposição, ISO, balanço de branco (com Kelvin quando aplicável),
    Picture Style e seus parâmetros, ALO.
  - **Rodapé de status:** estado da ponte (aguardando / ao vivo / sem câmera), fps de
    entrada/saída e tempo de processamento por quadro.
- **Com o painel aberto, a ponte liga o Live View** mesmo sem programa usando a webcam (o
  painel conta como consumidor), para dar para ajustar antes de uma chamada. Ao fechar, volta
  à regra normal (desliga 5 s depois de não haver consumidor).
- Novo comando `eoscam panel`: abre o painel do `eoscam-bg` já em execução (encontre a janela
  oculta dele e envie uma mensagem); se ele não estiver rodando, inicie-o e depois abra.
- O laço de captura passa a rodar numa thread própria no `eoscam-bg`; a thread principal roda
  a fila de mensagens. O `eoscam run` (console) usa o mesmo pipeline, sem bandeja nem painel.

## Fora do escopo

- Instalar a v2 em `C:\Program Files` (precisa de administrador; o usuário/orquestrador faz).
- Controles da câmera expostos aos outros programas via `IKsControl`.

## Critérios de aceite

1. Compila sem avisos (`/W4`) com `cmake --build build --config Release`.
2. Teste local (sem admin, ver "Como testar"): `eoscam test --direct --size 1920x1080` entrega
   ≥ 25 fps com Super Resolution + Artifact Reduction ligados no `settings.ini`, e o quadro
   salvo tem boa aparência (abra o JPG e confira: recorte pela base, cores, bordas da máscara).
3. Cada efeito, modo e modo de fundo testado ao menos uma vez, com um quadro salvo como evidência
   numa pasta temporária de testes.
4. Sem o Maxine (simule, por exemplo, com `NV_VIDEO_EFFECTS_PATH` apontando para uma pasta
   vazia), o pipeline cai para a CPU sem quebrar.
5. O painel abre e fecha, os ajustes refletem na prévia e persistem entre reinicializações, e a
   bandeja funciona. Faça uma captura de tela da janela (`PrintWindow`) e salve-a como evidência.
6. Ajustes da câmera: a lista de valores vem da câmera; mudar a compensação de exposição e o
   balanço de branco muda a imagem (compare quadros salvos).
7. README atualizado.

## Como testar sem administrador

- Antes de tudo, pare a ponte instalada (ela segura o mutex `Local\EosCamProducer`):
  `& "C:\Program Files\EosCam\eoscam.exe" stop`. **Ao terminar, religue-a:**
  `& "C:\Program Files\EosCam\eoscam.exe" start` (a webcam do usuário depende dela).
- Pipeline completo sem Frame Server: com `$env:EOSCAM_LOCAL='1'`, rode
  `build\Release\eoscam.exe run` em segundo plano e `build\Release\eoscam.exe test --direct`.
- A câmera deve estar ligada; ela já está conectada por USB.
- **Não** rode `scripts\deploy.ps1` nem nada que peça UAC.

# canon-eos-webcam: Canon EOS como webcam nativa do Windows 11

[English](README.md) · **Português (Brasil)**

Usa uma Canon EOS antiga (testado com a **EOS Rebel T3 / 1100D**) como webcam, sem os
drivers da Canon: só o driver PTP/WPD padrão do Windows e a API de câmera virtual do
Windows 11 (`MFCreateVirtualCamera`). A versão 2 processa a imagem na GPU com o
**NVIDIA Maxine** e traz um painel de controle com ícone na bandeja.

| | EOS Webcam Utility | canon-eos-webcam v2 |
|---|---|---|
| Resolução | ~480×360 | **1920×1080** (recorte 16:9 do Live View de 1056×704, com Super Resolution opcional) |
| Quadros/s | — | ~28 (o Live View da T3 entrega ~29) |
| CPU da ponte, sem efeitos de IA | alta | ~19% de um núcleo |
| Efeitos | — | Super Resolution, redução de artefatos, redução de ruído, remoção/troca de fundo, ajustes de imagem |
| Ajustes da câmera | — | exposição, ISO, balanço de branco, Picture Style, ALO, foco automático (pelo painel, via PTP) |
| Live View | sempre ligado | liga só quando algum programa usa a webcam (ou o painel está aberto) |

## Como funciona

```
Canon T3 ──USB/PTP (driver WPD)──> canon-eos-webcam-bg.exe / canon-eos-webcam run          (sessão do usuário)
                                    thread de captura: PTP, Live View, ajustes da câmera
                                       │ JPEG 1056×704 (sempre o mais recente)
                                    thread de processamento:
                                       JPEG → WIC → recorte 16:9 pela base (1056×594)
                                       → [Denoise] → [Artifact Reduction]       GPU (Maxine)
                                       → [Green Screen: máscara]                GPU (Maxine)
                                       → [Super Resolution 2×]                  GPU (Maxine)
                                       → ajustes + fundo + NV12 1920×1080       CPU
                                       │
                                  memória compartilhada Global\CanonEosWebcamFrames2
                                       │  quadros NV12 1920×1080 prontos
                                       ▼
                         canon-eos-webcam-source.dll  (carregada pelo Frame Server, LocalService)
                         só copia o quadro; desenha as telas de aviso
                                       │
                                       ▼
                 câmera virtual "Canon EOS Webcam"  →  Teams, Zoom, navegador, OBS...
```

- **`canon-eos-webcam-source.dll`**: fonte de mídia (IMFMediaSource) registrada em HKLM. O Frame
  Server a carrega quando um programa abre a webcam. Ela sinaliza no canal compartilhado
  que quer quadros; sem quadros, mostra uma tela explicando o que falta. Fica enxuta de
  propósito: não decodifica nada e **não carrega Maxine nem CUDA** dentro do Frame Server.
  Se a ponte for de outra versão (quadro de tamanho diferente), mostra "Versões
  incompatíveis do canon-eos-webcam: reinstale". O canal v2 (`CanonEosWebcamFrames2`) não se mistura com o da v1.
- **`canon-eos-webcam.exe` / `canon-eos-webcam-bg.exe`**: a ponte com a câmera e o pipeline de imagem. Liga o
  Live View (`SetRemoteMode`, `EVFOutputDevice = PC`) quando a fonte pede quadros (ou o painel
  está aberto), busca os quadros com `GetViewFinderData` e desliga o Live View 5 s depois que
  ninguém mais usa a webcam. Se outro software derrubar o Live View, religa automaticamente.
  O `canon-eos-webcam-bg.exe` é o mesmo código sem console, com **ícone na bandeja e painel**.
- **`tools/eosprobe`**: ferramenta de medição do Live View via PTP.

Formato oferecido: só NV12 1920×1080 a 30 fps, recorte 16:9 ancorado na base (descarta o topo).
Com várias opções, alguns programas abriam primeiro em 640×480 (4:3) e trocavam para 16:9 um
segundo depois.

## Efeitos de IA (NVIDIA Maxine)

Precisam do **runtime do NVIDIA Video Effects** (instalador do Maxine VFX SDK, "End-user
Redistributables") em `C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects` (ou na pasta da
variável `NV_VIDEO_EFFECTS_PATH`) e de uma GPU RTX; os modelos são específicos de cada arquitetura
(`SR_2x_*`, `AR-*`, `Denoise-*`, `AIGS_*` em `models\`). Nada é compilado contra o CUDA Toolkit: o
projeto só usa a API do SDK (`NvVFX_*`, `NvCVImage_*`), carregada dinamicamente
(`third_party/maxine`, licença MIT).

| Efeito | Modos | Observação |
|---|---|---|
| Super Resolution 2× | desligado / conservador / agressivo | 1056×594 → 2112×1188, depois reduz para 1920×1080. Desligado: aumento bilinear |
| Redução de artefatos | desligado / conservador / agressivo | na resolução de origem |
| Redução de ruído | desligado / conservador / agressivo | temporal (mantém estado); o SDK só aceita largura e altura múltiplas de 8, então roda nas 592 linhas de baixo |
| Remover o fundo (Green Screen) | ligado/desligado | máscara na resolução de origem, suavizada e ampliada na composição |
| Fundo | cor sólida, desfoque (0–100), imagem PNG/JPG | o desfoque usa o `BackgroundBlur` do SDK com a máscara; a imagem preenche 1920×1080 ("cover") |

Trocar de modo recria só o efeito afetado, "a quente": o quadro continua usando o modelo antigo
enquanto o novo carrega (centenas de ms). Ao abrir a webcam os efeitos ativos já são pré-carregados.
Sem o runtime (ou se a GPU falhar) os efeitos ficam indisponíveis (o painel mostra o motivo) e o
pipeline segue só na CPU: decodifica, recorta, escala, ajusta, converte.

**Ajustes de imagem** (sobre a imagem da câmera, antes do fundo): brilho, contraste, gamma,
temperatura (ganhos R/B), tom (verde/magenta) e saturação, aplicados por LUT.

## Painel de controle

Ícone na bandeja (menu: **Abrir painel**, **Iniciar com o Windows**, **Sair**; duplo clique abre o
painel) ou `canon-eos-webcam panel`. Com o painel aberto a ponte liga o Live View mesmo sem programa usando a
webcam, para dar para ajustar antes de uma chamada; ao fechar a janela (a ponte continua na
bandeja) volta à regra normal. Tem prévia ao vivo, grupos de **Imagem** (duplo clique num controle
zera), **Efeitos de IA**, **Fundo** e **Câmera**, e um rodapé com o estado da ponte, quadros/s de
entrada e saída e o tempo de processamento por quadro. As configurações ficam em
`%APPDATA%\CanonEosWebcam\settings.ini` (seções `[image]`, `[effects]`, `[background]`, `[ui]`) e são
gravadas ~0,5 s depois de cada mudança.

**Idioma.** A interface (painel, menu da bandeja, telas de aviso da câmera virtual e os comandos do
dia a dia) está em inglês e em português do Brasil. O idioma vem, nesta ordem, da variável
`CANON_EOS_WEBCAM_LANG` (`en` ou `pt`), da chave `language` na seção `[ui]` do `settings.ini` (`auto`, `en` ou
`pt`; vale ao iniciar a ponte) e do idioma de exibição do Windows (português → `pt`, qualquer outro →
`en`). As telas de aviso são desenhadas pela DLL dentro do Frame Server (conta LocalService), então
seguem o idioma do sistema. Enquanto a ponte ainda está iniciando, a câmera virtual mostra um quadro preto
(até 10 s) e, durante uma pausa curta na imagem ao vivo (por exemplo quando a câmera derruba o Live View
sozinha e a ponte o restaura), mantém o último quadro (até 8 s) em vez de piscar um aviso. Logs e saídas de diagnóstico ficam sempre em inglês.

**Câmera (PTP, os valores ficam guardados pela própria câmera, não pelo canon-eos-webcam).** A lista de valores
vem da câmera (`GetEvent`: `PropValueChanged` e `AvailListChanged`); só aparecem habilitados os ajustes
que o modo atual da câmera anuncia. Na T3 em modo automático: compensação de exposição, balanço de
branco (automático, luz do dia, sombra, nublado, tungstênio, fluorescente, flash, personalizado),
Picture Style e seus parâmetros (contraste, nitidez, saturação, tom de cor) funcionam; o ISO só
aparece nos modos P/Tv/Av/M, e o Auto Lighting Optimizer pode ser recusado conforme o modo ("a câmera
recusou"). A temperatura de cor em Kelvin só é oferecida se a câmera listar o modo de temperatura de
cor (a T3 não lista). Os parâmetros do Picture Style não têm tratamento no libgphoto2: a estrutura
(`0xD150`…`0xD155`, estilos do usuário em `0xD160`…) foi decifrada lendo e escrevendo na própria câmera.
O botão **Foco automático** envia `DoAf` (`0x9154`), o equivalente a apertar o disparador até a metade:
a chave da lente precisa estar em AF, e a câmera foca com o modo de AF do Live View (no "Modo rápido" o
espelho desce durante o foco; o "Modo ao vivo" mantém a imagem).

## Compilar

Requer Visual Studio 2022 (C++), CMake e o Windows SDK 10.0.22000+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Instalar

```powershell
# 1. Como administrador: copia para C:\Program Files\CanonEosWebcam e registra a DLL
pwsh -ExecutionPolicy Bypass -File scripts\deploy.ps1

# 2. Como usuário comum:
& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" install          # cria a câmera virtual
& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" autostart on     # ponte em segundo plano, inicia com o Windows
& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" panel            # abre o painel de controle
```

**Atualizando a partir do eoscam** (o nome antigo do projeto): o `deploy.ps1` para a ponte antiga e apaga
`C:\Program Files\EosCam`, o `install` remove a câmera virtual antiga "Canon EOS Webcam (eoscam)", o
`autostart on` remove a entrada antiga de inicialização, e as configurações são copiadas de
`%APPDATA%\EosCam` na primeira execução. As variáveis de ambiente agora começam com `CANON_EOS_WEBCAM_` em
vez de `EOSCAM_`.

A câmera deve estar ligada, num modo de foto (P/Av/Tv/M, para ter mais ajustes) e com o Live View
habilitado no menu. Se a webcam for aberta antes de qualquer programa, o canal `Global\` só passa a
existir quando a DLL é carregada pelo Frame Server; o painel funciona mesmo assim.

## Comandos

```
canon-eos-webcam run                  ponte neste console (para ver o log ao vivo; sem bandeja nem painel)
canon-eos-webcam start | stop         ponte em segundo plano (canon-eos-webcam-bg.exe, com bandeja e painel)
canon-eos-webcam panel                abre o painel (inicia o canon-eos-webcam-bg se ele não estiver rodando)
canon-eos-webcam autostart on|off     iniciar a ponte em segundo plano junto com o Windows
canon-eos-webcam install | uninstall  criar/remover a câmera virtual
canon-eos-webcam status               estado do canal (quadros, produtor, consumidor)
canon-eos-webcam list                 câmeras que o Windows enxerga
canon-eos-webcam test [--seconds N] [--size LxA] [--out f.jpg] [--direct]
                            abre a webcam como um app comum e mede quadros/s e CPU
canon-eos-webcam process in.jpg [--out f.jpg] [--bench N]
                            passa um JPEG pelo pipeline (sem câmera): testa efeitos e mede os tempos
canon-eos-webcam cam [--set PROP=VALOR]... [--out f.jpg]
                            diagnóstico (com a ponte parada): lista os ajustes que a câmera anuncia,
                            escreve propriedades (hexadecimal) e salva um quadro processado de cada vez
```

Logs: `C:\ProgramData\CanonEosWebcam\canon-eos-webcam.log` (ponte; a cada 60 s grava quadros/s, CPU e o tempo por
estágio) e `source.log` (fonte, dentro do Frame Server).

## Desempenho (RTX 4080, Live View da T3 a ~29 fps)

Medido com `canon-eos-webcam test --direct --size 1920x1080` e a câmera real; tempos por quadro em média móvel.

| Configuração | Quadros/s entregues | CPU da ponte | Por estágio (ms) |
|---|---|---|---|
| Sem efeitos de IA | 26–28 (limite da câmera) | ~19% de um núcleo | decodificação 2,6 + escala/ajustes/NV12 3,5 (CPU) |
| Super Resolution + Artifact Reduction | 28 | ~80% | GPU: SR 6,0, AR 1,8, envio 0,2, retorno 0,9; CPU: 2,7 + 4,1 |
| Tudo ligado (SR, AR, Denoise, fundo com desfoque) | 27,6 | ~165% | GPU: SR 5,7, AR 1,5, DN 1,7, máscara+desfoque 3,3, envio/retorno 1,3; CPU: 3,2 + 12,4 |

Sem efeitos a ponte cabe em menos de 25% de um núcleo (código AVX2 com alternativa em C++ puro).
Com a IA ligada o CPU sobe porque o SDK espera cada estágio da GPU de forma síncrona (a GPU mede 2–6 ms
por estágio na primeira medição e fica mais lenta quando está em repouso).

## Desenvolvimento

`canon-eos-webcam test --direct` carrega a DLL dentro do próprio processo, sem registro nem Frame
Server. Com `CANON_EOS_WEBCAM_LOCAL=1` no ambiente (no `run` e no `test`), o canal usa o namespace
`Local\`, então dá para testar tudo sem administrador:

```powershell
$env:CANON_EOS_WEBCAM_LOCAL = '1'
.\build\Release\canon-eos-webcam.exe run             # em um terminal (pare antes a ponte instalada: canon-eos-webcam stop)
.\build\Release\canon-eos-webcam.exe test --direct   # em outro
```

Variáveis de ambiente úteis: `CANON_EOS_WEBCAM_SETTINGS` (outro `settings.ini`), `CANON_EOS_WEBCAM_LOG` (log do
canon-eos-webcam-bg), `NV_VIDEO_EFFECTS_PATH` (outra pasta do runtime do Maxine; uma pasta vazia simula a
ausência dele), `CANON_EOS_WEBCAM_FORCE_CPU_BLUR=1` (usa o desfoque de fundo da CPU), `CANON_EOS_WEBCAM_THREADS`
(threads do pool de imagem), `CANON_EOS_WEBCAM_DUMP_EVENTS=1` (mostra no log os eventos de propriedades da
câmera), `CANON_EOS_WEBCAM_LANG=en|pt` (idioma da interface), `CANON_EOS_WEBCAM_EVF_REFRESH_S` /
`CANON_EOS_WEBCAM_EVF_ALT` (intervalo em segundos e valor temporário da troca de saída do Live View descrita abaixo;
`0` desliga a troca).

**O limite de 30 minutos do Live View.** A câmera encerra cada sessão de Live View 30 min depois de
iniciada, o que baixa e levanta o espelho e pausa a imagem por ~2 s enquanto a ponte religa o Live View. A cada 5 min a ponte
troca a saída do Live View para a tela da câmera e de volta para o PC (`EVFOutputDevice` 1 → 2, ~80 ms,
sem pausa visível), o que reinicia esse contador; testado na T3 por 33 min sem queda. Ela também envia
`KeepDeviceOn` (`0x911D`) a cada 30 s (a câmera aceita); o teste foi feito com os dois, então ainda não se
sabe se um deles sozinho basta.

Os textos da interface ficam em `src/common/i18n.h`, cada um com a versão em inglês e em português
lado a lado; para acrescentar um texto, acrescente uma linha `X(Id, "English", "Português")` na lista.

Para atualizar a DLL instalada, rode `scripts\deploy.ps1` de novo (como admin): ele para o
Frame Server para liberar o arquivo.

## Desinstalar

```powershell
& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" autostart off
& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" stop
& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" uninstall
pwsh -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Uninstall   # como admin
```

## Conflito com o EOS Webcam Utility

O driver do EOS Webcam Utility (UMDF, usa o EDSDK) fala com a câmera sempre que o Frame
Server do Windows entra em atividade e derruba o Live View do canon-eos-webcam. O canon-eos-webcam se recupera
sozinho em menos de 1 s, mas o ideal é desativar o dispositivo "EOS Webcam Utility" no
Gerenciador de Dispositivos ou desinstalar o EOS Webcam Utility.

## Licenças

O código do canon-eos-webcam é distribuído sob a licença MIT (veja `LICENSE`). `third_party/maxine` contém cabeçalhos e proxies do NVIDIA Maxine
Video Effects SDK (MIT, `third_party/maxine/LICENSE`); o runtime e os modelos do Maxine são
instalados à parte, pelo instalador da NVIDIA, sob a licença deles.

O canon-eos-webcam é um projeto independente, sem nenhuma ligação com a Canon ou a NVIDIA nem endosso delas.
Canon, EOS, NVIDIA e Maxine são marcas dos seus respectivos donos e aparecem aqui só para indicar
compatibilidade.

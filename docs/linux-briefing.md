# Briefing: eoscam para Linux (Omarchy)

## Objetivo

Fazer a Canon EOS Rebel T3 / 1100D aparecer como webcam no **Google Chrome / Google Meet** no Omarchy (Arch +
Hyprland), com um único programa nosso que:

- fala **direto com a câmera pelo protocolo PTP** (comandos de fabricante da Canon), via **libusb**;
- decodifica o JPEG do Live View, recorta em 16:9 e entrega **1280×720 a ~30 fps**;
- publica a imagem como uma **câmera virtual do PipeWire** (nó `Video/Source`), sem módulo de kernel;
- tem um **painel de ajustes** com prévia ao vivo, como o da versão para Windows. Ele cuida de duas coisas: os
  **ajustes de imagem** feitos pelo eoscam (brilho, contraste, gamma, temperatura, tom e saturação) e os
  **ajustes da câmera**, gravados na própria Canon via PTP (compensação de exposição, ISO, balanço de branco,
  temperatura de cor, Picture Style com contraste/nitidez/saturação/tom de cor e Auto Lighting Optimizer).

O que o substitui: a cadeia atual `gphoto2 --capture-movie | ffmpeg | v4l2loopback`, que quebra a cada
atualização (módulo DKMS do v4l2loopback, gvfs pegando a câmera, três processos ligados por pipe sem recuperação).

**Proibido usar:** gphoto2/libgphoto2, ffmpeg, v4l2loopback, akvcam, DKMS ou qualquer módulo de kernel. As únicas
dependências permitidas são bibliotecas de espaço de usuário com ABI estável dos repositórios oficiais do Arch:
`libusb` (1.0), `libjpeg-turbo` (API TurboJPEG) e `libpipewire` (0.3) no serviço, mais **`gtk4`** só no painel
(o serviço não pode depender do GTK). O compilador e o CMake são dependências de compilação.

## Contexto: a versão para Windows

Já existe uma versão funcionando para Windows 11 neste mesmo repositório (`eoscam`). Ela usa o driver PTP/WPD
padrão do Windows e o `MFCreateVirtualCamera`. Os arquivos de referência são:

- `src/app/canon_camera.cpp` / `.h`: os comandos PTP da Canon e a lógica de Live View/recuperação. **É a
  referência principal.** Lá o transporte é WPD; aqui passa a ser USB bulk via libusb, mas a sequência de
  comandos, os parâmetros e o parsing são os mesmos.
- `src/app/bridge.cpp`: as threads de captura e processamento, o Live View sob demanda e a reconexão.
- `src/app/pipeline.cpp` / `imaging.cpp`: o recorte 16:9 ancorado na base, os ajustes de imagem (`BuildAdjust`
  e `AdjustPixel`), a escala e a conversão para YUV.
- `src/app/panel.cpp`: o painel (controles, nomes dos valores da câmera, Picture Style). `src/app/settings.*`: a
  gravação das configurações com debounce e o recarregamento quando o arquivo é editado por fora.
- `README.md` e `CLAUDE.md`: visão geral e convenções.

Não altere nada do código do Windows. O código para Linux fica numa pasta própria (`linux/`), com o seu próprio
`CMakeLists.txt`, sem depender do CMake da raiz.

## Convenções

- C++20 (GCC ou Clang), compilando sem avisos com `-Wall -Wextra`.
- **Idioma:** comentários, mensagens de log, mensagens de erro, README e mensagens de commit são em **português do
  Brasil**, como no restante do projeto.
- **Interface:** o usuário trabalha nesta mesma máquina. Nunca use entrada sintética (ydotool, wtype, xdotool e
  afins), nunca roube o foco e nunca abra janelas ou diálogos sem ele pedir. Para testar, use o próprio programa,
  logs, `pw-dump`/`pw-cli` e ferramentas de linha de comando. Repita esta regra no prompt de qualquer subagente.
- Faça commits pequenos, um por etapa concluída e testada.

## Arquitetura

Dois programas:

- **`eoscam`**: o serviço, rodando como **serviço de usuário do systemd** (`systemctl --user`). Faz todo o
  trabalho e é o único que fala com a câmera.
- **`eoscam-panel`**: o painel de ajustes (GTK4), aberto pelo usuário quando quer. Não fala com a câmera nem
  processa imagem: conversa com o serviço por um socket Unix (ver "Canal de controle"). Fechar o painel ou ele
  travar não afeta a webcam.

O serviço tem quatro threads, no mesmo espírito da versão para Windows:

1. **Captura**: é a **única dona da sessão USB/PTP**. Conecta, liga o Live View, busca os JPEGs em loop, lê os
   eventos da câmera de tempos em tempos e reconecta sozinha se algo der errado. Cada JPEG novo vai para uma
   "caixa de correio" de um item só (sobrescreve o anterior). **Entre um quadro e outro**, ela executa os
   comandos de escrita de propriedades enfileirados pelo painel. Nenhuma outra thread toca na câmera.
2. **Processamento**: pega sempre o JPEG **mais recente** e descarta os outros. Assim a captura segue a ~30 fps
   mesmo se o processamento atrasar. Faz decodificação → recorte → escala → YUV e entrega o quadro à saída.
3. **PipeWire**: o `pw_thread_loop` com o stream de saída.
4. **Controle**: atende o socket do painel.

### Live View sob demanda

Como no Windows, o Live View **só fica ligado enquanto alguém consome a câmera**. Conta como consumidor o
Chrome (via PipeWire) **ou o painel aberto** (para a prévia e os ajustes da câmera funcionarem mesmo sem
nenhuma chamada em andamento):

- quando o stream do PipeWire passa para `PW_STREAM_STATE_STREAMING` (um consumidor, como o Chrome, ligou),
  a captura conecta e liga o Live View;
- quando ele sai de `STREAMING` (o consumidor fechou), espere **5 s** e então desligue o Live View e encerre a
  sessão PTP. Isso evita ligar e desligar quando o Meet troca de câmera ou recarrega.

Sem consumidor, o processo fica ocioso, com CPU ≈ 0.

Enquanto a câmera não estiver disponível (desligada, desconectada ou o primeiro quadro ainda não chegou), mande
um quadro de espera, por exemplo cinza-escuro liso. Assim o Chrome não fica com a tela preta "travada" e a
câmera não some da lista.

## Protocolo: PTP sobre USB (libusb)

### Encontrar e abrir a câmera

- Fabricante Canon: `idVendor = 0x04a9`. Procure a interface de classe **6** (Still Image), subclasse 1,
  protocolo 1. Ela tem um endpoint bulk IN, um bulk OUT e um interrupt IN (este último não é necessário: a Canon
  entrega os eventos pelo comando `GetEvent`).
- Faça `libusb_set_auto_detach_kernel_driver(h, 1)` e depois `libusb_claim_interface`. Se der `LIBUSB_ERROR_BUSY`,
  outro processo está com a câmera. Normalmente é o `gvfsd-gphoto2` ou um gphoto2 que sobrou; o log deve dizer
  isso em português claro, sem só repetir o código de erro.
- Use a API de hotplug da libusb (se `libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)`) ou procure a câmera a cada
  2 s enquanto há consumidor e ela não está conectada.

### Formato do contêiner PTP (little-endian)

```
u32 tamanho total (cabeçalho de 12 bytes + carga)
u16 tipo: 1 = comando, 2 = dados, 3 = resposta, 4 = evento
u16 código (operação ou resposta)
u32 transaction id (incrementa a cada operação; a mesma em comando, dados e resposta)
... parâmetros u32 (até 5) ou dados
```

Fluxo de uma operação:

1. envia o contêiner de **comando** pelo bulk OUT;
2. se houver dados **para a câmera** (ex.: `SetDevicePropValueEx`): envia um contêiner de **dados** (cabeçalho
   tipo 2 com o mesmo código + carga) pelo bulk OUT;
3. se houver dados **vindos da câmera** (ex.: `GetViewFinderData`, `GetEvent`): lê pelo bulk IN. O primeiro
   pacote traz o cabeçalho com o tamanho total; continue lendo até completar. Se o tamanho for múltiplo do
   `wMaxPacketSize`, a câmera manda um pacote de tamanho zero no fim, que também precisa ser consumido;
4. lê o contêiner de **resposta** (tipo 3) pelo bulk IN. O código `0x2001` significa OK.

Use buffers de leitura grandes (≥ 512 KiB; um quadro do Live View tem ~60–150 KiB) e timeouts curtos (~2 s).
Em caso de timeout ou erro de I/O, faça `libusb_clear_halt` nos endpoints. Se não resolver, use a requisição de
classe **Device Reset** (`bmRequestType 0x21`, `bRequest 0x66`) ou `libusb_reset_device`, e reconecte do zero.

### Sequência da Canon (copiada da versão para Windows, já testada nesta câmera)

Códigos:

| Nome | Código |
|---|---|
| `OpenSession` | `0x1002` (parâmetro: id da sessão, ex. 1) |
| `CloseSession` | `0x1003` |
| `SetDevicePropValueEx` (Canon) | `0x9110` (sem parâmetros; dados: `[u32 12][u32 prop][u32 valor]`) |
| `SetRemoteMode` (Canon) | `0x9114` (parâmetro: 1 liga, 0 desliga) |
| `SetEventMode` (Canon) | `0x9115` (parâmetro: 1 liga, 0 desliga) |
| `GetEvent` (Canon) | `0x9116` (sem parâmetros; dados vindos da câmera) |
| `GetViewFinderData` (Canon) | `0x9153` (parâmetros: `0x00100000, 0, 0`; dados vindos da câmera) |
| Propriedade `EVFOutputDevice` | `0xD1B0` (2 = PC, 0 = desligado) |
| Propriedade `EVFMode` | `0xD1B3` (1) |
| Resposta OK | `0x2001` |
| Resposta `SessionAlreadyOpen` | `0x201E` (trate como sucesso) |
| Resposta `DeviceBusy` | `0x2019` (quadro ainda não pronto: tente de novo) |
| Resposta Canon "não pronto" | `0xA102` (idem) |

**Ligar o Live View** (deve ser idempotente; a mesma função serve para religá-lo):

```
OpenSession(1)                      // só ao conectar
SetRemoteMode(1)                    // se falhar, a conexão falhou
SetEventMode(1)
GetEvent                            // o primeiro despeja o estado completo; pode descartar ou interpretar
SetDevicePropValueEx(EVFMode = 1)
SetDevicePropValueEx(EVFOutputDevice = 2)   // se falhar, a conexão falhou
```

**Desligar** (ao desconectar):

```
SetDevicePropValueEx(EVFOutputDevice = 0)
SetEventMode(0)
SetRemoteMode(0)
CloseSession
```

Desligue também ao receber SIGTERM/SIGINT, para a câmera não ficar presa em modo remoto.

**Loop de captura:**

- `GetViewFinderData(0x00100000, 0, 0)`. Se a resposta for `0xA102` ou `0x2019`, espere ~3 ms e tente de novo;
  isso não conta como erro. Mais de 10 erros seguidos de outro tipo: desconecte e reconecte do zero.
- Os dados vêm em blocos `[u32 tamanho][u32 tipo][dados]`. O JPEG é o bloco de **tipo 1** cujos dados começam com
  `FF D8`. Ignore os outros blocos.
- A cada ~1 s, chame `GetEvent` para esvaziar a fila de eventos da câmera. Se ela encher, a câmera para de
  responder. O buffer é uma sequência de registros `[u32 tamanho][u32 tipo]...` terminada por um registro de
  tamanho 8 e tipo 0. Por enquanto não é preciso interpretar nada.
- **Recuperação:** a partir de 2,5 s depois de conectar (o primeiro quadro leva ~1,2 s), se passar mais de
  800 ms sem quadro bom, chame de novo a sequência "ligar o Live View" (no máximo uma vez por segundo). Depois de
  3 religamentos sem nenhum quadro bom, desconecte e reconecte do zero.

O quadro do Live View da T3 é um JPEG de **1056×704** a ~30 fps.

## Processamento de imagem

1. Decodifique com a TurboJPEG (`tj3Decompress8` ou equivalente, em RGB, ou direto para YUV planar com
   `tj3DecompressToYUVPlanes8`, o que for mais simples e barato).
2. **Recorte 16:9 ancorado na base:** a largura inteira (1056) e as **594** linhas de baixo, descartando as 110
   de cima (normalmente parede/teto). A posição do recorte (base/centro/topo) deve ser uma opção de configuração.
3. **Ajustes de imagem** (ver a seção própria), aplicados na resolução do recorte, que é menor que a de saída e
   portanto mais barata. Pule a etapa quando todos os ajustes estiverem no padrão.
4. Escale para **1280×720** (bilinear basta) e converta para o formato do PipeWire.
5. Se a decodificação de um quadro falhar, descarte-o sem derrubar nada.

A 720p isso é leve para a CPU. Meça e registre no log a média de quadros/s e o tempo por quadro a cada 60 s,
como faz a versão para Windows. Deixe o pipeline organizado em etapas (decodificação → recorte → [efeitos] →
escala → conversão), com um ponto claro para inserir efeitos depois (ver "Futuro: Maxine").

## Câmera virtual no PipeWire

- `pw_stream` com `PW_DIRECTION_OUTPUT` e as propriedades:
  - `media.class = "Video/Source"`
  - `media.role = "Camera"` (o portal de câmera do xdg-desktop-portal, usado pelo Chrome, filtra por isso)
  - `node.name = "eoscam"`, `node.description = "Canon EOS (eoscam)"`
- Formato fixo: **1280×720, 30/1 fps**. Comece com `SPA_VIDEO_FORMAT_YUY2`; se o Chrome não aceitar, ofereça
  também `I420`/`NV12`. Teste qual o Chrome escolhe e deixe só o necessário. Na versão para Windows, oferecer um
  formato só evitou dor de cabeça.
- O ritmo dos quadros: o stream pode ser o *driver* do grafo (`PW_STREAM_FLAG_DRIVER` + `pw_stream_trigger_process`
  num timer de 30 Hz), entregando sempre o último quadro processado (repetido, se não chegou um novo). Use a
  abordagem que funcionar de forma estável com o Chrome e documente a escolha.
- Use `state_changed` do stream para o Live View sob demanda (ver "Arquitetura").

**No Chrome:** o suporte a câmera via PipeWire pode precisar da flag `chrome://flags/#enable-webrtc-pipewire-camera`
("PipeWire Camera support") ativada. Verifique na versão instalada e documente no README. Confirme também que o
`xdg-desktop-portal` está rodando e que, ao abrir o Meet, aparece "Canon EOS (eoscam)" na lista de câmeras. Peça
ao usuário para fazer esse teste final no navegador, sem automatizar cliques.

## Ajustes de imagem (feitos pelo eoscam)

São os mesmos seis da versão para Windows, com as mesmas faixas e fórmulas, para um `settings.ini` valer nos
dois sistemas:

| Ajuste | Faixa | Padrão |
|---|---|---|
| Brilho | −100..+100 | 0 |
| Contraste | −100..+100 | 0 |
| Gamma | 0,50..2,00 | 1,00 |
| Temperatura (negativo = frio/azulado, positivo = quente) | −100..+100 | 0 |
| Tom (negativo = verde, positivo = magenta) | −100..+100 | 0 |
| Saturação | 0..200 % | 100 % |

Implementação (copiada de `BuildAdjust`/`AdjustPixel` em `src/app/imaging.cpp`): uma LUT de 256 entradas por
canal, recalculada só quando os ajustes mudam, mais a saturação em ponto fixo.

```
tg = temperatura/100 × 0,25        mg = tom/100 × 0,25
ganho R = 1 + tg     ganho G = 1 − mg     ganho B = 1 − tg
c  = contraste × 2,55
cf = 259 × (c + 255) / (255 × (259 − c))
br = brilho/100 × 0,4
para cada canal e i em 0..255:
    x = min(1, i/255 × ganho do canal)
    x = x ^ (1/gamma)
    x = (x − 0,5) × cf + 0,5 + br
    lut[i] = round(clamp(x, 0, 1) × 255)

por pixel: r,g,b = lut_r[r], lut_g[g], lut_b[b]
se saturação ≠ 100 %:
    s = round(saturação × 128 / 100)
    cinza = (54 r + 183 g + 19 b) >> 8
    cada canal = clamp(cinza + ((canal − cinza) × s >> 7))
```

Os valores ficam em `~/.config/eoscam/settings.ini`, seção `[image]`, com as chaves `brightness`, `contrast`,
`gamma`, `temperature`, `tint` e `saturation` (as mesmas do Windows). O serviço grava ~0,5 s depois da última
mudança (debounce), e uma edição feita à mão no arquivo é recarregada sozinha (inotify). Assim dá para testar
tudo sem o painel. Valores ausentes ou inválidos voltam ao padrão.

## Ajustes da câmera (PTP)

Estes **não** vão para o `settings.ini`: ficam guardados na própria câmera. Os valores possíveis vêm da câmera,
e **só ficam habilitados os ajustes que o modo atual (a roda de modos da câmera) anuncia**.

### Leitura: eventos do `GetEvent`

O buffer do `GetEvent` (`0x9116`) é uma sequência de registros `[u32 tamanho][u32 tipo][dados]`, terminada
por um registro de tamanho 8 e tipo 0. Interessam dois tipos (offsets contados do início do registro):

- **`0xC189` PropValueChanged**: `[8] u32 código da propriedade`, `[12] u32 valor` (ou uma estrutura, no caso do
  Picture Style). Guarde o valor e também os bytes crus de `[12]` até o fim do registro.
- **`0xC18A` AvailListChanged**: `[8] u32 código`, `[12] u32 tipo de dado`, `[16] u32 quantidade`,
  `[20] valores`, 4 bytes cada. É a lista de valores permitidos no modo atual.

Mantenha um cache `código → {valor, lista, bytes crus}` com um contador de geração que sobe a cada mudança. O
primeiro `GetEvent` depois de ligar o modo de eventos traz o estado completo. Com o painel aberto, leia os
eventos a cada **250 ms** (em vez de 1 s), para o painel refletir rápido o que mudar na câmera, inclusive pelos
botões dela.

`EOSCAM_DUMP_EVENTS=1` deve imprimir todos os eventos `0xC189`/`0xC18A` em hexadecimal, para diagnóstico.

### Escrita

`SetDevicePropValueEx` (`0x9110`), dados `[u32 12][u32 propriedade][u32 valor]`. Para o Picture Style, dados
`[u32 tamanho total][u32 propriedade][palavras u32...]`. A resposta `0x2001` significa aceito. Qualquer outra
significa que a câmera recusou (é normal em alguns modos), e o painel deve mostrar "a câmera recusou" em vez de
um código. Depois de uma escrita aceita, atualize o cache de forma otimista; o `PropValueChanged` seguinte
confirma ou corrige.

### Propriedades e codificações (conferidas na T3)

| Ajuste | Propriedade | Valores |
|---|---|---|
| Compensação de exposição | `0xD104` | byte com sinal em 1/8 de EV; a câmera usa ±3 e ±5 para 1/3 e 2/3. Mostrar como "−1 2/3 EV", "0", "+1/3 EV" (arredondar `n×3/8` para terços) |
| ISO | `0xD103` | `0x00` e `0x01` = Auto, `0x48` = 100, `0x50` = 200, `0x58` = 400, `0x60` = 800, `0x68` = 1600, `0x70` = 3200, `0x78` = 6400 (tabela completa em `panel.cpp`, `kIsoNames`). Só aparece nos modos P/Tv/Av/M; nos outros, mostrar "(automático)" |
| Balanço de branco | `0xD109` | 0 Automático, 1 Luz do dia, 8 Sombra, 2 Nublado, 3 Tungstênio, 4 Fluorescente, 5 Flash, 6 Personalizado, 9 Temperatura de cor (K), 10–12 Personalizado PC-1..3 |
| Temperatura de cor | `0xD10A` | Kelvin, 2500..10000. Habilitado só se a lista do balanço de branco tiver o valor 9 **e** ele estiver selecionado (a T3 não lista o 9, então aparece desabilitado) |
| Picture Style | `0xD110` | use só o byte baixo: `0x81` Padrão, `0x82` Retrato, `0x83` Paisagem, `0x84` Neutro, `0x85` Fiel, `0x86` Monocromático, `0x87` Automático, `0x88` Detalhe fino, `0x21`–`0x23` Definido pelo usuário 1–3 |
| Auto Lighting Optimizer | `0xD1C1` | 0 Padrão, 1 Baixa, 2 Forte, 3 Desativada. Mostrar só se a câmera informar um valor < `0x100`; pode ser recusado conforme o modo |

Nomes desconhecidos aparecem em hexadecimal (`0x4B`), sem esconder o valor.

### Parâmetros do Picture Style (decifrados na própria câmera; o libgphoto2 não trata)

Cada estilo tem a sua propriedade de parâmetros: `0x81`→`0xD150`, `0x82`→`0xD151`, `0x83`→`0xD152`,
`0x84`→`0xD153`, `0x85`→`0xD154`, `0x86`→`0xD155`, `0x21`→`0xD160`, `0x22`→`0xD161`, `0x23`→`0xD162`.

O valor (os bytes crus do `PropValueChanged`, a partir do offset 12) é uma lista de palavras `u32`. Ele tem
**28 bytes** nos estilos de fábrica, com os parâmetros a partir da palavra **1**, e **32 bytes** nos estilos do
usuário (há uma palavra a mais com o estilo base), com os parâmetros a partir da palavra **2**. A partir daí,
as palavras são, nesta ordem e como inteiros com sinal:

| Parâmetro | Faixa |
|---|---|
| Contraste | −4..+4 |
| Nitidez | 0..7 |
| Saturação | −4..+4 |
| Tom de cor | −4..+4 |

Para mudar um parâmetro: pegue a propriedade de parâmetros do estilo **atual**, copie **todas** as palavras do
cache, troque só a palavra do parâmetro e escreva a lista inteira de volta com `SetDevicePropValueEx`. Se o
estilo atual não tiver parâmetros conhecidos ou o cache tiver menos de 28 bytes, deixe os quatro controles
desabilitados.

## Canal de controle (serviço ↔ painel)

Socket Unix em `$XDG_RUNTIME_DIR/eoscam.sock`, com permissão só para o usuário. Use um protocolo de texto
simples, uma mensagem por linha (ex.: JSON por linha), para dar para testar com `socat`. Ele precisa cobrir:

- **estado**: câmera conectada ou não, nome dela, estado (ocioso / ligando / ao vivo / sem câmera), quadros/s
  de entrada e de saída, se o Chrome está consumindo;
- **ajustes de imagem**: ler e alterar (o serviço aplica no próximo quadro e grava no `settings.ini`);
- **ajustes da câmera**: o cache inteiro (valor, lista permitida e bytes crus por propriedade) com o número de
  geração, avisos quando ele muda, e pedidos de escrita (que entram na fila da thread de captura e devolvem
  "aceito" ou "a câmera recusou");
- **prévia**: com o painel inscrito, o serviço envia o quadro **final** (depois dos ajustes, o mesmo que o
  Chrome recebe), reduzido para ~640×360, a no máximo 15 fps. Por exemplo, em JPEG recomprimido com a TurboJPEG
  (barato e pequeno). Pare de enviar quando o painel sair;
- **painel aberto**: enquanto houver um painel conectado, ele conta como consumidor para o Live View sob
  demanda.

Se o painel não ler a tempo, o serviço descarta quadros da prévia: o painel nunca pode atrasar a webcam.

## Painel de ajustes (`eoscam-panel`, GTK4)

Uma janela normal (o Hyprland a gerencia como qualquer outra), com uma entrada `.desktop` ("Ajustes da webcam
Canon") para abrir pelo lançador do Omarchy. Uma segunda instância só traz a primeira para a frente. Tudo em
português.

Layout, como no Windows:

- **Prévia** à esquerda, ao vivo, com a proporção 16:9 preservada e zoom (Ajustar / 100 % / 200 % / 300 %, roda
  do mouse para zoom, arrastar para mover, duplo clique para ajustar). É para conferir foco e nitidez.
- **Imagem**: os seis controles deslizantes, com o valor ao lado (gamma com duas casas, saturação em %), e o
  botão "Restaurar padrões". A imagem muda enquanto o usuário arrasta.
- **Câmera (ajustes guardados pela própria câmera)**: uma linha de informação ("Canon EOS 1100D (só os ajustes
  que o modo atual permite)" ou "Câmera desconectada ou desligada: ajustes indisponíveis."); listas para
  compensação de exposição, ISO, balanço de branco, Picture Style e Auto Lighting Optimizer, preenchidas com a
  **lista anunciada pela câmera**; controles deslizantes para temperatura de cor e para os quatro parâmetros do
  Picture Style. Os deslizantes da câmera **só enviam ao soltar** (cada escrita é uma operação USB). Controles
  que o modo atual não anuncia ficam desabilitados ("n/d"). O painel se atualiza sozinho quando a geração do
  cache muda (inclusive se o usuário mexer na roda de modos da câmera).
- **Barra de estado**: estado da câmera, quadros/s e se a webcam está em uso.
- Um grupo "Efeitos de IA (NVIDIA Maxine)" **não** entra agora (ver "Futuro"), mas deixe espaço no layout para
  ele.

Se o serviço não estiver rodando, o painel mostra isso e tenta reconectar sozinho, sem travar.

**Testes do painel sem entrada sintética:** a lógica (montar as listas, nomes, codificações, Picture Style)
fica fora dos widgets e pode ser testada pelo canal de controle e pelos subcomandos. Para conferir o visual,
se possível, faça o painel renderizar a si mesmo num PNG sem mostrar a janela (ex.: `eoscam-panel
--snapshot arquivo.png`, usando `gtk_widget_snapshot` e o renderizador do GSK); se não der, peça ao usuário para
abrir o painel e olhar. Nunca simule cliques ou teclas.

## Permissões, udev e conflitos

- Arquivo `/etc/udev/rules.d/70-eoscam.rules`, em `/etc` para nenhuma atualização de pacote sobrescrever:
  - `SUBSYSTEM=="usb", ATTR{idVendor}=="04a9", TAG+="uaccess"` (acesso ao usuário da sessão, sem root nem grupos).
  - Esconder a câmera do gvfs: o monitor de volumes gphoto2 do gvfs se baseia na propriedade `ID_GPHOTO2` do udev.
    Verifique se o gvfs (`gvfs-gphoto2`) está instalado. Se estiver, avalie zerar essa propriedade para a Canon
    numa regra que rode depois das do libgphoto2 (ex.: `99-eoscam-nogvfs.rules`). Confirme que funciona (o
    `claim_interface` não dá mais BUSY com o gerenciador de arquivos aberto) antes de adotar.
- **Antes de mexer no sistema:** levante o que existe da solução antiga (módulo `v4l2loopback` carregado via
  `/etc/modules-load.d` ou `/etc/modprobe.d`, serviços do systemd com gphoto2/ffmpeg, scripts no autostart do
  Hyprland). Mostre a lista ao usuário e **pergunte antes de desativar ou remover** qualquer coisa. Os dois não
  podem rodar juntos, porque só um processo pode ter a câmera.

## Instalação e serviço

- Instale em `~/.local/bin/eoscam`. Serviço em `~/.config/systemd/user/eoscam.service`, com `Restart=on-failure`,
  `WantedBy=default.target` e dependência do `pipewire.service`. Nada de configuração dentro dos arquivos do
  Omarchy (`~/.config/hypr`, etc.), para as atualizações dele não interferirem.
- O serviço fica **sempre rodando** (barato sem consumidor) e o nó do PipeWire existe sempre, então a câmera
  aparece no Chrome mesmo antes de a Canon ser ligada.
- Configuração em `~/.config/eoscam/settings.ini`: seção `[image]` (ajustes de imagem, ver acima) e seção
  `[linux]` (posição do recorte, formato, nível de log). Edições externas são recarregadas sozinhas.
- O painel instala `~/.local/bin/eoscam-panel` e `~/.local/share/applications/eoscam-panel.desktop`.
- Log no journal (stdout/stderr com `systemd`), em português.
- Um `scripts/install.sh` (no `linux/`) que compila, instala o binário, a unit e a regra do udev (esta última com
  `sudo`, avisando o usuário antes) e roda `udevadm control --reload` + `systemctl --user enable --now eoscam`.

## Subcomandos (para testar sem o navegador)

- `eoscam probe`: acha a câmera, abre a sessão, liga o Live View, salva **um** JPEG em `/tmp/eoscam-probe.jpg`,
  desliga tudo e mostra um resumo (dispositivo, endpoints, tempos).
- `eoscam bench [segundos]`: captura e processa sem PipeWire e mede quadros/s de captura, tempo de
  processamento e CPU.
- `eoscam run`: o serviço em primeiro plano (o que a unit do systemd executa).
- `eoscam process in.jpg out.yuv`: roda o pipeline sobre um JPEG, sem câmera, com os ajustes de imagem do
  `settings.ini`.
- `eoscam cam`: com o serviço parado, conecta e lista todas as propriedades da câmera conhecidas (código, nome,
  valor atual, lista permitida e bytes crus), como o `eoscam cam` do Windows.
- `eoscam get` / `eoscam set <propriedade> <valor>`: falam com o serviço em execução pelo socket (ex.:
  `eoscam set image.brightness 20`, `eoscam set cam.wb 1`, `eoscam set cam.style.sharpness 5`). Servem para
  testar os ajustes sem o painel.

## Etapas e critérios de aceite

1. **PTP básico:** `eoscam probe` salva um JPEG válido de 1056×704 da T3. Rodar o `probe` 10 vezes seguidas
   funciona todas as vezes (a câmera sai do modo remoto direito).
2. **Captura contínua:** `eoscam bench 60` mantém **≥ 28 fps** de captura. Desconectar e reconectar o cabo e
   desligar e religar a câmera durante o teste se recuperam sozinhos, sem reiniciar o processo.
3. **Pipeline:** `eoscam process` gera um quadro 1280×720 com o recorte certo (verificar convertendo o `.yuv` com
   alguma ferramenta já instalada ou escrevendo um PNG de depuração).
4. **PipeWire:** o nó aparece em `pw-dump`/`wpctl status` com o nome e a classe certos. Um consumidor de teste
   (ex.: `gst-launch-1.0 pipewiresrc target-object=eoscam ! fakesink`, se o GStreamer estiver instalado) recebe
   quadros a ~30 fps. Sem consumidor, o Live View desliga em ~5 s e a CPU do processo fica ≈ 0.
5. **Chrome/Meet:** "Canon EOS (eoscam)" aparece no Meet e a imagem fica fluida (teste feito pelo usuário).
6. **Ajustes de imagem:** `eoscam process` com valores conhecidos no `settings.ini` dá o resultado esperado
   (compare pixels com as fórmulas). Editar o arquivo com o serviço rodando muda a imagem em menos de 1 s, e
   `eoscam set image.*` grava no arquivo. Medir o custo: com ajustes ligados, a captura continua ≥ 28 fps.
7. **Ajustes da câmera:** `eoscam cam` lista as propriedades da tabela. Pelo `eoscam set`, mudar balanço de
   branco, compensação de exposição, Picture Style e um parâmetro dele muda a imagem e é confirmado pelo
   `PropValueChanged`. Um valor recusado volta a mensagem "a câmera recusou", sem derrubar a captura.
8. **Painel:** prévia ao vivo com zoom, os seis ajustes de imagem e os ajustes da câmera habilitados conforme o
   modo. Abrir o painel sem nenhuma chamada liga o Live View; fechar desliga em ~5 s. O painel aberto não reduz
   os quadros/s da webcam. O teste final de uso é feito pelo usuário.
9. **Robustez:** o serviço sobe sozinho no login, sobrevive a `pacman -Syu` com troca de kernel (não há nada
   compilado contra o kernel) e se recupera de reinício do PipeWire (`systemctl --user restart pipewire`).
10. **README** em `linux/README.md`: instalação, flag do Chrome, painel, configuração, como diagnosticar
    (`probe`, `cam`, journal) e como remover.

## Futuro: Maxine (não fazer agora)

O NVIDIA Maxine Video Effects SDK tem versão para Linux (voltada a Ubuntu/RHEL e contêineres, com CUDA/TensorRT).
Mais tarde dá para ligar Denoise, Artifact Reduction, fundo virtual/desfoque e Super Resolution, como no Windows.
Por isso:

- deixe a etapa de efeitos como uma interface no pipeline (recebe e devolve uma imagem RGB/BGR do recorte, antes
  da escala), hoje sem nenhuma implementação;
- no painel, o grupo "Efeitos de IA (NVIDIA Maxine)" e o "Fundo virtual" entram junto com os efeitos. As chaves
  do `settings.ini` serão as mesmas do Windows (seções `[effects]` e `[background]`), então hoje o serviço
  apenas preserva essas seções ao gravar o arquivo;
- não linke nada contra CUDA. Na versão para Windows o runtime é carregado dinamicamente (`dlopen` no Linux) e,
  se faltar, os efeitos ficam indisponíveis sem derrubar o pipeline. Mantenha esse padrão quando chegar a hora.

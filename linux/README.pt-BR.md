# canon-eos-webcam para Linux

Transforma uma Canon EOS (testada: Rebel T3 / 1100D) em uma **câmera virtual do PipeWire** que o Chrome, o Google
Meet e qualquer aplicativo PipeWire/portal pode usar. Fala com a câmera direto pela USB (PTP, via libusb), então
**não precisa de gphoto2, ffmpeg, v4l2loopback nem módulo de kernel**. English: [README.md](README.md).

Dependências (todas dos repositórios oficiais do Arch, ABIs estáveis): `libusb`, `libjpeg-turbo` e `libpipewire` no
serviço; além delas `gtk4` no painel de ajustes. Ferramentas de build: `cmake`, compilador C++20, `pkgconf`.

## Instalação

```bash
sudo pacman -S --needed cmake libusb libjpeg-turbo pipewire gtk4   # dependências de build e execução
linux/scripts/install.sh
```

O `install.sh` compila, instala `canon-eos-webcam` e `canon-eos-webcam-panel` em `~/.local/bin`, a unit de usuário do
systemd, a entrada do lançador ("Ajustes da webcam Canon") e inicia o serviço. Também oferece instalar a regra do
udev **com `sudo`** (avisa antes; pule com `--no-udev`). A regra costuma ser desnecessária: o systemd já dá ao usuário
logado acesso às câmeras PTP. Confira que `~/.local/bin` está no `PATH`.

O serviço fica sempre rodando (não custa nada em repouso) e a câmera **Canon EOS Webcam** sempre existe no PipeWire,
mesmo antes de ligar a Canon: nesse caso mostra um quadro cinza-escuro. O Live View só liga enquanto alguém usa a
câmera (um consumidor do PipeWire, como o Chrome, ou o painel aberto) e desliga 5 s depois que o último sai.

**Só um programa pode ter a câmera por vez.** Pare qualquer cadeia antiga de gphoto2 / v4l2loopback antes de iniciar
esta. Se outro programa estiver com a câmera, o log diz qual.

### Chrome / Google Meet

O Chrome lê as câmeras pelo xdg-desktop-portal quando o suporte a câmera via PipeWire está ligado. Abra
`chrome://flags/#enable-webrtc-pipewire-camera`, coloque em **Enabled** e reinicie o Chrome. O `xdg-desktop-portal`
precisa estar rodando (está, no Omarchy). Aí "Canon EOS Webcam" aparece na lista de câmeras do Meet. (Confira o portal
com `busctl --user get-property org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop org.freedesktop.portal.Camera IsCameraPresent`,
que deve responder `b true` enquanto o serviço roda.)

A imagem é 1280×720 a 30 fps em um único formato de pixel (YUY2 por padrão).

## Painel de ajustes

Rode `canon-eos-webcam-panel` (ou "Ajustes da webcam Canon" no lançador). À esquerda: pré-visualização ao vivo (a
roda do mouse ou os botões dão zoom 100 / 200 / 300 %, arrastar move, duplo clique ajusta; com zoom ele pede o quadro
completo de 1280 de largura, para conferir o foco). À direita: os seis **ajustes de imagem** feitos pelo
canon-eos-webcam (brilho, contraste, gamma, temperatura, tom, saturação) e os **ajustes da câmera**, gravados na
própria Canon (compensação de exposição, ISO, balanço de branco, temperatura de cor, Picture Style com contraste /
nitidez / saturação / tom de cor, Auto Lighting Optimizer), além de um botão **Foco automático**. Só o que o modo
atual da câmera permite fica habilitado. Os sliders da câmera enviam quando você solta. Abrir uma segunda vez só traz a primeira janela para a frente. Abrir o
painel liga o Live View; fechar desliga 5 s depois. O painel nunca afeta a webcam: se ele travar, o serviço continua.

**Foco automático** foca uma vez, como apertar o botão do obturador até a metade. A chave da lente precisa estar em
**AF**. A câmera usa o modo de AF do Live View (menu da câmera): no **modo Ao vivo** a imagem continua; no **modo
Rápido** o espelho desce enquanto foca e a imagem pausa.

`canon-eos-webcam-panel --snapshot saida.png [--preview quadro.jpg]` desenha o painel com dados de demonstração em um
PNG sem mostrar janela (serve para conferir o layout).

## Configuração

`~/.config/canon-eos-webcam/settings.ini` (o mesmo formato do arquivo da versão Windows; as seções que este programa
não conhece são preservadas). As edições valem em menos de um segundo com o serviço rodando.

```ini
[image]            ; mesmas chaves e faixas do Windows
brightness = 0     ; -100..100
contrast = 0       ; -100..100
gamma = 1.00       ; 0.50..2.00
temperature = 0    ; -100..100
tint = 0           ; -100..100
saturation = 100   ; 0..200
[ui]
language = auto    ; auto | en | pt
[linux]
crop = bottom      ; bottom | center | top : qual faixa 16:9 do quadro 3:2 do Live View é usada
format = yuy2      ; yuy2 | i420 | nv12 : formato de pixel do PipeWire (lido na partida)
loglevel = info    ; info | debug
```

Idioma: `CANON_EOS_WEBCAM_LANG` (`en`|`pt`), depois `[ui] language`, depois `LANG` / `LC_MESSAGES`.
Outras variáveis: `CANON_EOS_WEBCAM_SETTINGS` (outro arquivo de ajustes), `CANON_EOS_WEBCAM_SOCKET`,
`CANON_EOS_WEBCAM_DUMP_EVENTS=1` (registra cada evento de propriedade da câmera em hexadecimal),
`CANON_EOS_WEBCAM_EVF_REFRESH_S` / `CANON_EOS_WEBCAM_EVF_ALT` (intervalo em segundos e valor temporário da troca de
saída do Live View que evita o limite de 30 minutos da câmera; padrão 300 e 1, `0` desliga),
`CANON_EOS_WEBCAM_KEEPALIVE=0` (para o `KeepDeviceOn` enviado a cada 30 s; só para testes).

A câmera encerra cada sessão de Live View 30 minutos depois de começar, o que baixaria o espelho e pausaria a imagem
por ~2 s. O serviço evita isso sem mudar nenhum ajuste da câmera: manda `KeepDeviceOn` a cada 30 s e, a cada 5
minutos, troca a saída do Live View para a tela da câmera e de volta para o PC (~80 ms, sem pausa visível), o que
reinicia esse cronômetro. Em pausas curtas com a câmera conectada (o Live View sendo religado, um foco automático), a
câmera virtual repete a última imagem por até 8 s em vez do quadro cinza.

## Linha de comando

| Comando | |
|---|---|
| `canon-eos-webcam run` | o serviço em primeiro plano (o que o systemd inicia) |
| `canon-eos-webcam get` | estado do serviço em execução (câmera, fps, ajustes de imagem, propriedades da câmera) |
| `canon-eos-webcam set <chave> <valor>` | `image.{brightness,contrast,gamma,temperature,tint,saturation}`, `cam.{exp,iso,wb,kelvin,style,alo}`, `cam.style.{contrast,sharpness,saturation,tone}`, `cam.autofocus 1` (foca uma vez); os valores da câmera aceitam números (`0x81`) ou nomes em inglês (`daylight`, `standard`, ISO `400`) |
| `canon-eos-webcam probe` | abre a câmera, salva um quadro em `/tmp/canon-eos-webcam-probe.jpg`, desliga tudo |
| `canon-eos-webcam bench [segundos]` | captura + processamento sem PipeWire: fps, ms por quadro, CPU |
| `canon-eos-webcam process in.jpg out.yuv [debug.jpg]` | roda o pipeline sobre um JPEG, sem câmera |
| `canon-eos-webcam cam` | (serviço parado) lista todas as propriedades da câmera com valores permitidos e bytes brutos |

## Diagnóstico

* `journalctl --user -u canon-eos-webcam -f` (estatísticas a cada 60 s; problemas de conexão dizem em palavras simples
  o que houve, por exemplo qual processo está com a câmera ou que falta a regra do udev).
* `canon-eos-webcam probe` e `canon-eos-webcam cam` testam a câmera sem PipeWire (pare o serviço antes:
  `systemctl --user stop canon-eos-webcam`).
* `pw-dump | grep -B3 -A12 canon-eos-webcam` ou `wpctl status`: o nó é `Video/Source`, papel `Camera`.
* Consumidor de teste: `gst-launch-1.0 pipewiresrc ! fakesink` (alguns WirePlumber recusam o link automático de um
  consumidor GStreamer; `pw-link canon-eos-webcam:capture_1 <consumidor>:input_1` liga à mão).

## Como funciona

O `canon-eos-webcam run` tem quatro threads: **captura** (a única dona da sessão USB/PTP: conecta, busca os JPEGs do
Live View a ~30 fps, lê os eventos da câmera, executa as gravações de propriedades enfileiradas entre os quadros e
reconecta sozinha), **processamento** (sempre pega o JPEG mais recente: decodifica com TurboJPEG → recorte 16:9 →
ajustes de imagem → escala bilinear para 1280×720 → YUV; há um estágio vazio para os efeitos de IA da versão
Windows), **PipeWire** (o stream é o driver do grafo, disparado por um timer de 30 Hz só enquanto há consumidor
ligado; repete o último quadro ou manda um quadro de espera) e **controle** (um socket Unix,
`$XDG_RUNTIME_DIR/canon-eos-webcam.sock`, um objeto JSON por linha, usado pelo painel e por `get`/`set`; também vigia
o `settings.ini` com inotify). O painel é um programa GTK4 separado que só fala com esse socket
(`socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/canon-eos-webcam.sock`, depois `{"cmd":"get"}`). Os comandos da Canon e o
tratamento do Picture Style são os da versão Windows (`src/app/canon_camera.cpp`, `src/app/panel.cpp`).

## Remover

```bash
linux/scripts/uninstall.sh
```

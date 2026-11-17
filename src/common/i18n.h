// User-interface strings in English (the default) and Brazilian Portuguese.
//
// Every string lives in the CANON_EOS_WEBCAM_STRINGS list below as X(Id, "English", "Português"), so a string can
// never be missing in one of the languages. Logs and diagnostic output stay in English; only what the
// user sees (control panel, tray menu, warning screens of the virtual camera and the everyday console
// commands) goes through Tr()/TrW().
//
// Language: CANON_EOS_WEBCAM_LANG (en|pt), then "language" in the [ui] section of settings.ini (auto|en|pt), then
// the Windows display language (Portuguese -> pt, anything else -> en). The DLL runs as LocalService, so
// it only sees the environment and display language of that account (normally the system language).
//
// Strings are UTF-8 (the project compiles with /utf-8). Placeholders: printf specifiers in the console
// and panel formats (same specifiers, in the same order, in both languages); {arg} and {sub} in the
// texts built through Msg.
#pragma once

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <string>

namespace canon_eos_webcam {

enum class Lang : int { En = 0, Pt = 1 };

// clang-format off
#define CANON_EOS_WEBCAM_STRINGS(X) \
    X(None, "", "") \
    /* ---- tray icon and panel window */ \
    X(TrayTip, "canon-eos-webcam — Canon EOS as a webcam", "canon-eos-webcam — Canon EOS como webcam") \
    X(TrayOpen, "Open panel", "Abrir painel") \
    X(TrayAutostart, "Start with Windows", "Iniciar com o Windows") \
    X(TrayExit, "Exit", "Sair") \
    X(PanelTitle, "canon-eos-webcam — control panel", "canon-eos-webcam — painel de controle") \
    X(WaitingForCamera, "Waiting for the camera image...", "Aguardando imagem da câmera...") \
    X(ZoomLabel, "Zoom:", "Zoom:") \
    X(ZoomFit, "Fit", "Ajustar") \
    X(ZoomHint, "Mouse wheel: zoom · drag: pan · double-click: fit", \
                "Roda do mouse: zoom · arrastar: mover · duplo clique: ajustar") \
    /* ---- AI effects */ \
    X(GroupEffects, "AI effects (GPU, NVIDIA Maxine)", "Efeitos de IA (GPU, NVIDIA Maxine)") \
    X(FxSuperRes, "Super Resolution 2×", "Super Resolution 2×") \
    X(FxArtifact, "Artifact reduction", "Redução de artefatos") \
    X(FxDenoise, "Noise reduction", "Redução de ruído") \
    X(LevelOff, "Off", "Desligado") \
    X(LevelCon, "Conservative", "Conservador") \
    X(LevelAgg, "Aggressive", "Agressivo") \
    X(FxStateOff, "off", "desligado") \
    X(FxStateActive, "active", "ativo") \
    X(FxStateActiveCon, "active (conservative)", "ativo (conservador)") \
    X(FxStateActiveAgg, "active (aggressive)", "ativo (agressivo)") \
    X(FxStateLoadingModel, "loading the model...", "carregando o modelo...") \
    X(FxStateLoadingMode, "loading the new mode...", "carregando o novo modo...") \
    X(FxStateFailed, "failed: {arg}", "falhou: {arg}") \
    X(UnavailablePrefix, "unavailable: ", "indisponível: ") \
    X(MaxineNotInitialized, "the NVIDIA Maxine runtime has not been initialized yet", \
                            "o runtime do NVIDIA Maxine ainda não foi inicializado") \
    X(MaxineNotFound, "NVIDIA Video Effects runtime not found in {arg}", \
                      "runtime do NVIDIA Video Effects não encontrado em {arg}") \
    X(MaxineDllFailed, "couldn't load NVVideoEffects.dll ({arg})", "não consegui carregar NVVideoEffects.dll ({arg})") \
    X(MaxineCudaFailed, "couldn't create the CUDA stream ({arg}); is there an NVIDIA GPU and driver?", \
                        "não consegui criar o stream CUDA ({arg}); GPU NVIDIA ou driver ausente?") \
    /* ---- virtual background */ \
    X(GroupBackground, "Virtual background (AI Green Screen)", "Fundo virtual (AI Green Screen)") \
    X(BgRemove, "Remove the background", "Remover o fundo") \
    X(BgReplaceWith, "Replace with", "Substituir por") \
    X(BgModeSolid, "Solid color", "Cor sólida") \
    X(BgModeBlur, "Blur", "Desfoque") \
    X(BgModeImage, "Image", "Imagem") \
    X(BgColorButton, "Color…", "Cor…") \
    X(BgBlurStrength, "Blur strength", "Intensidade do desfoque") \
    X(BgImageButton, "Image…", "Imagem…") \
    X(BgNoImageChosen, "(no image chosen)", "(nenhuma imagem escolhida)") \
    X(BgPickTitle, "Choose the background image", "Escolher a imagem de fundo") \
    X(BgPickFilterImages, "Images (PNG, JPG, BMP)", "Imagens (PNG, JPG, BMP)") \
    X(BgPickFilterAll, "All files", "Todos os arquivos") \
    X(BgNoteNoImage, "no background image chosen", "nenhuma imagem de fundo escolhida") \
    X(BgNoteOpenFailed, "couldn't open the background image", "não consegui abrir a imagem de fundo") \
    X(BgNoteReadFailed, "failed to read the background image", "falha ao ler a imagem de fundo") \
    /* ---- image adjustments */ \
    X(GroupImage, "Image", "Imagem") \
    X(ImgBrightness, "Brightness", "Brilho") \
    X(ImgContrast, "Contrast", "Contraste") \
    X(ImgGamma, "Gamma", "Gamma") \
    X(ImgTemperature, "Temperature", "Temperatura") \
    X(ImgTint, "Tint (green/magenta)", "Tom (verde/magenta)") \
    X(ImgSaturation, "Saturation", "Saturação") \
    X(ImgReset, "Restore defaults", "Restaurar padrões") \
    /* ---- camera settings */ \
    X(GroupCamera, "Camera (settings stored in the camera itself)", "Câmera (ajustes guardados pela própria câmera)") \
    X(CamExpComp, "Exposure compensation", "Compensação de exposição") \
    X(CamIso, "ISO", "ISO") \
    X(CamWhiteBalance, "White balance", "Balanço de branco") \
    X(CamColorTemp, "Color temperature", "Temperatura de cor") \
    X(CamPictureStyle, "Picture Style", "Picture Style") \
    X(CamPsContrast, "  Contrast", "  Contraste") \
    X(CamPsSharpness, "  Sharpness", "  Nitidez") \
    X(CamPsSaturation, "  Saturation", "  Saturação") \
    X(CamPsColorTone, "  Color tone", "  Tom de cor") \
    X(CamAlo, "Auto Lighting Optimizer", "Auto Lighting Optimizer") \
    X(CamAutoFocus, "Autofocus", "Foco automático") \
    X(CamDisconnected, "Camera disconnected or turned off: settings unavailable.", \
                       "Câmera desconectada ou desligada: ajustes indisponíveis.") \
    X(CamModeHint, " (only the settings the current mode allows)", " (só os ajustes que o modo atual permite)") \
    X(CamIsoAuto, "(auto)", "(automático)") \
    X(NotAvailable, "n/a", "n/d") \
    /* names used inside the camera error messages */ \
    X(LabelExpComp, "exposure compensation", "compensação de exposição") \
    X(LabelIso, "ISO", "ISO") \
    X(LabelWhiteBalance, "white balance", "balanço de branco") \
    X(LabelColorTemp, "color temperature", "temperatura de cor") \
    X(LabelPictureStyle, "Picture Style", "Picture Style") \
    X(LabelPsParam, "Picture Style parameter", "parâmetro do Picture Style") \
    X(LabelAlo, "Auto Lighting Optimizer", "Auto Lighting Optimizer") \
    X(LabelAutoFocus, "autofocus", "foco automático") \
    X(CamErrNotConnected, "The camera is not connected: couldn't set the {sub}.", \
                          "A câmera não está conectada: não deu para ajustar {sub}.") \
    X(CamErrRefused, "The camera refused the setting ({sub}): response {arg}. Automatic mode?", \
                     "A câmera recusou o ajuste ({sub}): resposta {arg}. Modo automático?") \
    X(CamErrAutoFocus, "The camera couldn't autofocus: response {arg}. Is the lens switch on AF?", \
                       "A câmera não conseguiu focar: resposta {arg}. A chave da lente está em AF?") \
    /* white balance (Canon names) */ \
    X(WbAuto, "Auto", "Automático") \
    X(WbDaylight, "Daylight", "Luz do dia") \
    X(WbShade, "Shade", "Sombra") \
    X(WbCloudy, "Cloudy", "Nublado") \
    X(WbTungsten, "Tungsten", "Tungstênio") \
    X(WbFluorescent, "Fluorescent", "Fluorescente") \
    X(WbFlash, "Flash", "Flash") \
    X(WbCustom, "Custom", "Personalizado") \
    X(WbColorTemp, "Color temperature (K)", "Temperatura de cor (K)") \
    X(WbPc1, "Custom PC-1", "Personalizado PC-1") \
    X(WbPc2, "Custom PC-2", "Personalizado PC-2") \
    X(WbPc3, "Custom PC-3", "Personalizado PC-3") \
    /* Picture Style (Canon names) */ \
    X(PsStandard, "Standard", "Padrão") \
    X(PsPortrait, "Portrait", "Retrato") \
    X(PsLandscape, "Landscape", "Paisagem") \
    X(PsNeutral, "Neutral", "Neutro") \
    X(PsFaithful, "Faithful", "Fiel") \
    X(PsMonochrome, "Monochrome", "Monocromático") \
    X(PsAuto, "Auto", "Automático") \
    X(PsFineDetail, "Fine Detail", "Detalhe fino") \
    X(PsUser1, "User Def. 1", "Definido pelo usuário 1") \
    X(PsUser2, "User Def. 2", "Definido pelo usuário 2") \
    X(PsUser3, "User Def. 3", "Definido pelo usuário 3") \
    /* Auto Lighting Optimizer */ \
    X(AloStandard, "Standard", "Padrão") \
    X(AloLow, "Low", "Baixa") \
    X(AloStrong, "Strong", "Forte") \
    X(AloDisabled, "Disabled", "Desativada") \
    /* ---- status bar */ \
    X(StatusBridge, "Bridge: ", "Ponte: ") \
    X(StatusLive, "live", "ao vivo") \
    X(StatusStartingLiveView, "starting Live View", "ligando o Live View") \
    X(StatusStartingCamera, "starting the camera", "ligando a câmera") \
    X(StatusNoCamera, "no camera (turned on? USB?)", "sem câmera (ligada? USB?)") \
    X(StatusIdle, "waiting", "aguardando") \
    X(StatusStopped, "stopped", "parada") \
    X(StatusFps, "Input %.1f fps  |  output %.1f fps", "Entrada %.1f fps  |  saída %.1f fps") \
    X(StatusProcessing, "Processing %.1f ms/frame (GPU %.1f, CPU %.1f)", \
                        "Processamento %.1f ms/quadro (GPU %.1f, CPU %.1f)") \
    /* ---- warning screens of the virtual camera (DLL) */ \
    X(SrcInitializing, "Initializing...", "Inicializando...") \
    X(SrcNotRunningTitle, "canon-eos-webcam is not running", "O canon-eos-webcam não está em execução") \
    X(SrcNotRunningDetail, "Start the bridge with \"canon-eos-webcam start\" (or turn on \"canon-eos-webcam autostart on\").", \
                           "Inicie a ponte com \"canon-eos-webcam start\" (ou ative \"canon-eos-webcam autostart on\").") \
    X(SrcNoCameraTitle, "Canon camera not found", "Câmera Canon não encontrada") \
    X(SrcNoCameraDetail, "Check that the camera is turned on, in photo mode, and connected via USB.", \
                         "Verifique se a câmera está ligada, no modo de foto, e conectada via USB.") \
    X(SrcWaitingTitle, "Waiting for image...", "Aguardando imagem...") \
    X(SrcStartingTitle, "Starting the camera...", "Ligando a câmera...") \
    X(SrcStartingDetail, "Starting the Canon Live View.", "Iniciando o Live View da Canon.") \
    X(SrcIncompatibleTitle, "Incompatible canon-eos-webcam versions", "Versões incompatíveis do canon-eos-webcam") \
    X(SrcIncompatibleDetail, "The bridge (canon-eos-webcam.exe) and the virtual camera are from different versions: reinstall canon-eos-webcam.", \
                             "A ponte (canon-eos-webcam.exe) e a câmera virtual são de versões diferentes: reinstale o canon-eos-webcam.") \
    /* ---- console */ \
    X(CliOtherInstance, "Another canon-eos-webcam instance is already running (use \"canon-eos-webcam stop\" to end it).", \
                        "Outra instância do canon-eos-webcam já está rodando (use \"canon-eos-webcam stop\" para encerrá-la).") \
    X(CliAlreadyRunning, "canon-eos-webcam is already running.\n", "O canon-eos-webcam já está rodando.\n") \
    X(CliStartFailed, "Couldn't start %s (error %lu).\n", "Não consegui iniciar %s (erro %lu).\n") \
    X(CliStarted, "canon-eos-webcam started in the background (log in %s\\canon-eos-webcam.log).\n", \
                  "canon-eos-webcam iniciado em segundo plano (log em %s\\canon-eos-webcam.log).\n") \
    X(CliNotRunning, "canon-eos-webcam is not running.\n", "O canon-eos-webcam não está rodando.\n") \
    X(CliStopTimeout, "canon-eos-webcam did not respond to the stop request.\n", "O canon-eos-webcam não respondeu ao pedido de parada.\n") \
    X(CliStopped, "canon-eos-webcam stopped.\n", "canon-eos-webcam encerrado.\n") \
    X(CliRegWriteFailed, "Failed to write to the registry (error %ld).\n", "Falha ao gravar no registro (erro %ld).\n") \
    X(CliAutostartOn, "canon-eos-webcam will start with Windows (%s).\n", "O canon-eos-webcam vai iniciar junto com o Windows (%s).\n") \
    X(CliRegDeleteFailed, "Failed to remove from the registry (error %ld).\n", \
                          "Falha ao remover do registro (erro %ld).\n") \
    X(CliAutostartOff, "canon-eos-webcam will no longer start with Windows.\n", "O canon-eos-webcam não vai mais iniciar com o Windows.\n") \
    X(CliAutostartUsage, "usage: canon-eos-webcam autostart on|off\n", "uso: canon-eos-webcam autostart on|off\n") \
    X(CliInstallFailed, "MFCreateVirtualCamera failed (hr=0x%08lX).\n", "MFCreateVirtualCamera falhou (hr=0x%08lX).\n") \
    X(CliDllNotRegistered, "The source DLL is not registered. Run scripts\\deploy.ps1 as administrator.\n", \
                           "A DLL da fonte não está registrada. Rode scripts\\deploy.ps1 como administrador.\n") \
    X(CliVcamStartFailed, "IMFVirtualCamera::Start failed (hr=0x%08lX).\n", "IMFVirtualCamera::Start falhou (hr=0x%08lX).\n") \
    X(CliInstalled, "Virtual camera \"%s\" registered.\n", "Câmera virtual \"%s\" registrada.\n") \
    X(CliUninstallFailed, "Failed to remove the virtual camera (hr=0x%08lX).\n", \
                          "Falha ao remover a câmera virtual (hr=0x%08lX).\n") \
    X(CliUninstalled, "Virtual camera removed.\n", "Câmera virtual removida.\n") \
    X(CliPanelConsoleOnly, "canon-eos-webcam is running without a window (\"canon-eos-webcam run\" in a console): the panel only exists in canon-eos-webcam-bg.\n", \
                           "O canon-eos-webcam está rodando sem janela (\"canon-eos-webcam run\" em console): o painel só existe no canon-eos-webcam-bg.\n") \
    X(CliPanelNoWindow, "canon-eos-webcam started, but its window did not appear.\n", "O canon-eos-webcam iniciou, mas a janela não apareceu.\n") \
    X(CliPanelOpened, "Panel opened.\n", "Painel aberto.\n") \
    X(CliUsage, \
        "usage: canon-eos-webcam [command]\n" \
        "  run                 (default) bridges the Canon camera and the virtual webcam, in this console\n" \
        "  start | stop        starts/stops the bridge in the background (canon-eos-webcam-bg.exe)\n" \
        "  autostart on|off    starts (or not) the background bridge together with Windows\n" \
        "  install             registers the virtual camera \"Canon EOS Webcam\"\n" \
        "  uninstall           removes the virtual camera\n" \
        "  panel               opens the control panel (starts canon-eos-webcam-bg if it is not running)\n" \
        "  cam [--set PROP=VALUE]... [--out f.jpg]\n" \
        "                      diagnostics: lists the settings the camera announces and tests writing them (hex),\n" \
        "                      saving frames\n" \
        "  process in.jpg [--out f.jpg] [--bench N]\n" \
        "                      runs a JPEG through the image pipeline (no camera): tests effects and measures times\n" \
        "  status              shows the state of the channel between the app and the virtual camera\n" \
        "  list                lists the video cameras Windows can see\n" \
        "  test [--seconds N] [--size WxH] [--out file.jpg] [--direct]\n" \
        "                      opens the virtual webcam like a regular app and measures performance\n" \
        "                      --direct loads the DLL in this process (no Frame Server; use CANON_EOS_WEBCAM_LOCAL=1\n" \
        "                      both here and in \"canon-eos-webcam run\")\n" \
        "\n" \
        "Language: CANON_EOS_WEBCAM_LANG=en|pt, or language=auto|en|pt in the [ui] section of settings.ini.\n", \
        "uso: canon-eos-webcam [comando]\n" \
        "  run                 (padrão) faz a ponte entre a câmera Canon e a webcam virtual, neste console\n" \
        "  start | stop        inicia/encerra a ponte em segundo plano (canon-eos-webcam-bg.exe)\n" \
        "  autostart on|off    inicia (ou não) a ponte em segundo plano junto com o Windows\n" \
        "  install             registra a câmera virtual \"Canon EOS Webcam\"\n" \
        "  uninstall           remove a câmera virtual\n" \
        "  panel               abre o painel de controle (inicia o canon-eos-webcam-bg se ele não estiver rodando)\n" \
        "  cam [--set PROP=VALOR]... [--out f.jpg]\n" \
        "                      diagnóstico: lista os ajustes que a câmera anuncia e testa escrever (hex),\n" \
        "                      salvando quadros\n" \
        "  process in.jpg [--out f.jpg] [--bench N]\n" \
        "                      passa um JPEG pelo pipeline de imagem (sem câmera): testa efeitos e mede tempos\n" \
        "  status              mostra o estado do canal entre o app e a câmera virtual\n" \
        "  list                lista as câmeras de vídeo que o Windows enxerga\n" \
        "  test [--seconds N] [--size LxA] [--out arquivo.jpg] [--direct]\n" \
        "                      abre a webcam virtual como um app comum e mede o desempenho\n" \
        "                      --direct carrega a DLL neste processo (sem Frame Server; use CANON_EOS_WEBCAM_LOCAL=1\n" \
        "                      tanto aqui quanto no \"canon-eos-webcam run\")\n" \
        "\n" \
        "Idioma: CANON_EOS_WEBCAM_LANG=en|pt, ou language=auto|en|pt na seção [ui] do settings.ini.\n")
// clang-format on

enum class Str : int {
#define CANON_EOS_WEBCAM_STR_ID(id, en, pt) id,
    CANON_EOS_WEBCAM_STRINGS(CANON_EOS_WEBCAM_STR_ID)
#undef CANON_EOS_WEBCAM_STR_ID
    Count
};

namespace i18n_detail {

struct Entry {
    const char* text[2];  // indexed by Lang
};

inline constexpr Entry kTable[] = {
#define CANON_EOS_WEBCAM_STR_ENTRY(id, en, pt) {{en, pt}},
    CANON_EOS_WEBCAM_STRINGS(CANON_EOS_WEBCAM_STR_ENTRY)
#undef CANON_EOS_WEBCAM_STR_ENTRY
};
static_assert(sizeof kTable / sizeof kTable[0] == size_t(Str::Count));

inline std::atomic<int> g_lang{-1};  // -1 = not decided yet

inline bool ParseLang(const wchar_t* v, Lang& out) {
    if (_wcsnicmp(v, L"pt", 2) == 0) { out = Lang::Pt; return true; }
    if (_wcsnicmp(v, L"en", 2) == 0) { out = Lang::En; return true; }
    return false;  // empty, "auto" or unknown
}

inline Lang DisplayLang() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE ? Lang::Pt : Lang::En;
}

inline bool EnvLang(Lang& out) {
    wchar_t v[16];
    DWORD n = GetEnvironmentVariableW(L"CANON_EOS_WEBCAM_LANG", v, 16);
    return n > 0 && n < 16 && ParseLang(v, out);
}

}  // namespace i18n_detail

// Picks the language: CANON_EOS_WEBCAM_LANG, then [ui] language in the given settings.ini (may be nullptr), then the
// Windows display language. Call once at startup; without it the first Tr() uses CANON_EOS_WEBCAM_LANG or Windows.
inline Lang InitLang(const wchar_t* settingsPath) {
    Lang l;
    if (!i18n_detail::EnvLang(l)) {
        wchar_t v[16] = {};
        if (!settingsPath || !GetPrivateProfileStringW(L"ui", L"language", L"", v, 16, settingsPath) ||
            !i18n_detail::ParseLang(v, l))
            l = i18n_detail::DisplayLang();
    }
    i18n_detail::g_lang = int(l);
    return l;
}

inline Lang CurrentLang() {
    int v = i18n_detail::g_lang.load();
    return v < 0 ? InitLang(nullptr) : Lang(v);
}

// UTF-8 text of a string in the given language (or in the current one).
inline const char* Tr(Str id, Lang l) { return i18n_detail::kTable[int(id)].text[int(l)]; }
inline const char* Tr(Str id) { return Tr(id, CurrentLang()); }

// UTF-16 text (Win32 controls, GDI). Converted once, on first use, for both languages.
inline const wchar_t* TrW(Str id, Lang l) {
    struct Cache {
        std::wstring w[2][size_t(Str::Count)];
        Cache() {
            for (int lang = 0; lang < 2; lang++)
                for (size_t i = 0; i < size_t(Str::Count); i++) {
                    const char* s = i18n_detail::kTable[i].text[lang];
                    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
                    if (n <= 1) continue;
                    w[lang][i].resize(size_t(n - 1));
                    MultiByteToWideChar(CP_UTF8, 0, s, -1, w[lang][i].data(), n);
                }
        }
    };
    static const Cache cache;  // thread-safe initialization
    return cache.w[int(l)][int(id)].c_str();
}
inline const wchar_t* TrW(Str id) { return TrW(id, CurrentLang()); }

// A message built far from the UI (pipeline, bridge) and rendered only where it is shown: in the user's
// language on the panel, in English in the log. {arg} is replaced by arg and {sub} by the text of sub.
struct Msg {
    Str id = Str::None;
    std::string arg;
    Str sub = Str::None;

    Msg() = default;
    Msg(Str i, std::string a = {}, Str s = Str::None) : id(i), arg(std::move(a)), sub(s) {}
    bool empty() const { return id == Str::None; }
    bool operator==(const Msg& o) const { return id == o.id && arg == o.arg && sub == o.sub; }
};

inline std::string Text(const Msg& m, Lang l) {
    std::string s = Tr(m.id, l);
    auto replace = [&s](const char* key, const std::string& value) {
        size_t p = s.find(key);
        if (p != std::string::npos) s.replace(p, std::char_traits<char>::length(key), value);
    };
    replace("{arg}", m.arg);
    replace("{sub}", Tr(m.sub, l));
    return s;
}
inline std::string Text(const Msg& m) { return Text(m, CurrentLang()); }

}  // namespace canon_eos_webcam

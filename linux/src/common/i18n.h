// User-interface strings in English (the default) and Brazilian Portuguese, for the Linux port.
//
// Every string lives in the CANON_EOS_WEBCAM_STRINGS list as X(Id, "English", "Português"), so a string can never be
// missing in one of the languages. Logs and diagnostic output (probe, bench, cam, process) stay in English;
// only what the user sees (panel, .desktop entry, everyday commands) goes through Tr().
//
// Language: CANON_EOS_WEBCAM_LANG (en|pt), then "language" in the [ui] section of settings.ini (auto|en|pt), then the
// locale (LC_ALL / LC_MESSAGES / LANG starting with "pt" -> Portuguese).
// Placeholders: printf specifiers (same specifiers, in the same order, in both languages).
#pragma once

#include "ini.h"
#include "util.h"

#include <atomic>
#include <cstring>
#include <strings.h>
#include <string>

namespace canon_eos_webcam {

enum class Lang : int { En = 0, Pt = 1 };

// clang-format off
#define CANON_EOS_WEBCAM_STRINGS(X) \
    X(None, "", "") \
    /* ---- panel window */ \
    X(PanelTitle, "Canon webcam settings", "Ajustes da webcam Canon") \
    X(WaitingForCamera, "Waiting for the camera image...", "Aguardando imagem da câmera...") \
    X(ServiceNotRunning, "The canon-eos-webcam service is not running. Trying to reconnect...", \
                         "O serviço canon-eos-webcam não está rodando. Tentando reconectar...") \
    X(ZoomLabel, "Zoom:", "Zoom:") \
    X(ZoomFit, "Fit", "Ajustar") \
    X(ZoomHint, "Mouse wheel: zoom · drag: pan · double-click: fit", \
                "Roda do mouse: zoom · arrastar: mover · duplo clique: ajustar") \
    X(GroupEffectsSoon, "AI effects (NVIDIA Maxine): not available on Linux yet", \
                        "Efeitos de IA (NVIDIA Maxine): ainda indisponíveis no Linux") \
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
    X(CamPsContrast, "Contrast", "Contraste") \
    X(CamPsSharpness, "Sharpness", "Nitidez") \
    X(CamPsSaturation, "Saturation", "Saturação") \
    X(CamPsColorTone, "Color tone", "Tom de cor") \
    X(CamAlo, "Auto Lighting Optimizer", "Auto Lighting Optimizer") \
    X(CamDisconnected, "Camera disconnected or turned off: settings unavailable.", \
                       "Câmera desconectada ou desligada: ajustes indisponíveis.") \
    X(CamModeHint, "%s (only the settings the current mode allows)", "%s (só os ajustes que o modo atual permite)") \
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
    X(CamErrNotConnected, "The camera is not connected: couldn't set the %s.", \
                          "A câmera não está conectada: não deu para ajustar %s.") \
    X(CamErrRefused, "The camera refused the setting (%s). Automatic mode?", \
                     "A câmera recusou o ajuste (%s). Modo automático?") \
    X(CamErrOther, "Couldn't set the %s.", "Não deu para ajustar %s.") \
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
    X(StatusService, "Service: ", "Serviço: ") \
    X(StatusLive, "live", "ao vivo") \
    X(StatusStartingLiveView, "starting Live View", "ligando o Live View") \
    X(StatusStartingCamera, "starting the camera", "ligando a câmera") \
    X(StatusNoCamera, "no camera (turned on? USB?)", "sem câmera (ligada? USB?)") \
    X(StatusIdle, "waiting", "aguardando") \
    X(StatusStopped, "not running", "parado") \
    X(StatusFps, "Input %.1f fps  |  output %.1f fps", "Entrada %.1f fps  |  saída %.1f fps") \
    X(StatusProcessing, "Processing %.1f ms/frame", "Processamento %.1f ms/quadro") \
    X(StatusInUse, "webcam in use", "webcam em uso") \
    X(StatusNotInUse, "webcam not in use", "webcam sem uso") \
    /* ---- command line */ \
    X(CliUsage, \
      "canon-eos-webcam - Canon EOS as a webcam (PipeWire virtual camera)\n\n" \
      "Usage:\n" \
      "  canon-eos-webcam run                  run the service in the foreground (what systemd starts)\n" \
      "  canon-eos-webcam get                  show the state of the running service\n" \
      "  canon-eos-webcam set <key> <value>    change a setting of the running service\n" \
      "                                        image.{brightness,contrast,gamma,temperature,tint,saturation}\n" \
      "                                        cam.{exp,iso,wb,kelvin,style,alo}\n" \
      "                                        cam.style.{contrast,sharpness,saturation,tone}\n" \
      "  canon-eos-webcam probe                save one frame to /tmp/canon-eos-webcam-probe.jpg\n" \
      "  canon-eos-webcam bench [seconds]      capture + process without PipeWire and measure\n" \
      "  canon-eos-webcam process in.jpg out.yuv [debug.jpg]   run the pipeline over a JPEG\n" \
      "  canon-eos-webcam cam                  list the camera properties (service stopped)\n", \
      "canon-eos-webcam - Canon EOS como webcam (câmera virtual do PipeWire)\n\n" \
      "Uso:\n" \
      "  canon-eos-webcam run                  roda o serviço em primeiro plano (o que o systemd inicia)\n" \
      "  canon-eos-webcam get                  mostra o estado do serviço em execução\n" \
      "  canon-eos-webcam set <chave> <valor>  muda um ajuste do serviço em execução\n" \
      "                                        image.{brightness,contrast,gamma,temperature,tint,saturation}\n" \
      "                                        cam.{exp,iso,wb,kelvin,style,alo}\n" \
      "                                        cam.style.{contrast,sharpness,saturation,tone}\n" \
      "  canon-eos-webcam probe                salva um quadro em /tmp/canon-eos-webcam-probe.jpg\n" \
      "  canon-eos-webcam bench [segundos]     captura + processa sem PipeWire e mede\n" \
      "  canon-eos-webcam process in.jpg out.yuv [debug.jpg]   roda o pipeline sobre um JPEG\n" \
      "  canon-eos-webcam cam                  lista as propriedades da câmera (serviço parado)\n") \
    X(CliNoService, "The canon-eos-webcam service is not running (no socket at %s).\n", \
                    "O serviço canon-eos-webcam não está rodando (sem socket em %s).\n") \
    X(CliSetOk, "OK\n", "OK\n") \
    X(CliSetFailed, "Failed: %s\n", "Falhou: %s\n") \
    X(CliBadKey, "unknown setting '%s'", "ajuste desconhecido '%s'") \
    X(CliBadValue, "invalid value '%s'", "valor inválido '%s'") \
    X(CliNotConnected, "the camera is not connected", "a câmera não está conectada") \
    X(CliRefused, "the camera refused", "a câmera recusou") \
    X(CliTimeout, "the service did not answer", "o serviço não respondeu") \
    X(CliOutOfRange, "value out of range", "valor fora da faixa") \
    X(CliAlreadyRunning, "Another canon-eos-webcam service is already running (socket %s).\n", \
                         "Outro serviço canon-eos-webcam já está rodando (socket %s).\n")
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

inline bool ParseLang(const std::string& v, Lang& out) {
    if (v.size() >= 2 && strncasecmp(v.c_str(), "pt", 2) == 0) { out = Lang::Pt; return true; }
    if (v.size() >= 2 && strncasecmp(v.c_str(), "en", 2) == 0) { out = Lang::En; return true; }
    return false;  // empty, "auto" or unknown
}

inline Lang LocaleLang() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        std::string v = Env(name);
        if (v.empty()) continue;
        return strncasecmp(v.c_str(), "pt", 2) == 0 ? Lang::Pt : Lang::En;
    }
    return Lang::En;
}

}  // namespace i18n_detail

// Picks the language: CANON_EOS_WEBCAM_LANG, then [ui] language in the given settings file (may be empty), then the
// locale. Call once at startup; without it the first Tr() uses the environment and the locale.
inline Lang InitLang(const std::string& settingsPath) {
    Lang l;
    if (!i18n_detail::ParseLang(Env("CANON_EOS_WEBCAM_LANG"), l)) {
        IniDoc ini;
        if (settingsPath.empty() || !ini.Load(settingsPath) || !i18n_detail::ParseLang(ini.Get("ui", "language"), l))
            l = i18n_detail::LocaleLang();
    }
    i18n_detail::g_lang = int(l);
    return l;
}

inline Lang CurrentLang() {
    int v = i18n_detail::g_lang.load();
    return v < 0 ? InitLang("") : Lang(v);
}

inline const char* Tr(Str id, Lang l) { return i18n_detail::kTable[int(id)].text[int(l)]; }
inline const char* Tr(Str id) { return Tr(id, CurrentLang()); }

}  // namespace canon_eos_webcam

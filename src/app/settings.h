// Configurações do usuário (ajustes de imagem, efeitos de IA, fundo virtual e janela do painel),
// persistidas em %APPDATA%\EosCam\settings.ini (ou no arquivo apontado por EOSCAM_SETTINGS).
//
// Um único objeto compartilhado entre a thread do painel (que altera) e a thread de captura (que
// consulta uma vez por quadro). Cada alteração incrementa um contador de versão e agenda a gravação
// em disco com debounce de ~500 ms. Alterações feitas direto no arquivo (por exemplo, à mão) são
// recarregadas sozinhas, o que facilita testar sem o painel.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace eoscam {

enum class FxLevel : int { Off = 0, Con = 1, Agg = 2 };
enum class BgMode : int { Solid = 0, Blur = 1, Image = 2 };

// Ajustes de imagem (aplicados sobre a imagem da câmera, antes da composição com o fundo).
struct ImageSettings {
    int brightness = 0;    // -100..+100
    int contrast = 0;      // -100..+100
    float gamma = 1.0f;    // 0,5..2,0
    int temperature = 0;   // -100..+100 (negativo = frio/azulado, positivo = quente/alaranjado)
    int tint = 0;          // -100..+100 (negativo = verde, positivo = magenta)
    int saturation = 100;  // 0..200 (%)

    bool IsIdentity() const {
        return brightness == 0 && contrast == 0 && gamma == 1.0f && temperature == 0 && tint == 0 &&
               saturation == 100;
    }
    bool operator==(const ImageSettings& o) const {
        return brightness == o.brightness && contrast == o.contrast && gamma == o.gamma &&
               temperature == o.temperature && tint == o.tint && saturation == o.saturation;
    }
};

struct EffectSettings {
    FxLevel superRes = FxLevel::Off;
    FxLevel artifact = FxLevel::Off;
    FxLevel denoise = FxLevel::Off;
    bool operator==(const EffectSettings& o) const {
        return superRes == o.superRes && artifact == o.artifact && denoise == o.denoise;
    }
};

struct BackgroundSettings {
    bool greenScreen = false;
    BgMode mode = BgMode::Blur;
    uint32_t color = 0x00FF00;  // 0xRRGGBB
    int blur = 60;              // 0..100
    std::wstring imagePath;
    bool operator==(const BackgroundSettings& o) const {
        return greenScreen == o.greenScreen && mode == o.mode && color == o.color && blur == o.blur &&
               imagePath == o.imagePath;
    }
};

struct UiSettings {
    int x = -1, y = -1;     // posição da janela do painel (-1 = padrão do Windows)
};

struct Settings {
    ImageSettings image;
    EffectSettings fx;
    BackgroundSettings bg;
    UiSettings ui;
};

class SettingsStore {
public:
    SettingsStore();
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // Caminho do arquivo: EOSCAM_SETTINGS, senão %APPDATA%\EosCam\settings.ini.
    static std::wstring DefaultPath();

    // Lê o arquivo (valores ausentes ou inválidos ficam no padrão) e inicia a thread de gravação.
    void Load(const std::wstring& path);

    // Cópia consistente das configurações e do contador de versão.
    Settings Get(uint32_t* version = nullptr) const;
    uint32_t Version() const;

    // Aplica uma alteração (sob o mutex), sobe a versão e agenda a gravação.
    void Update(const std::function<void(Settings&)>& fn);

    // Recarrega se o arquivo mudou no disco por fora (chamar de vez em quando, ex.: 1x por segundo).
    void ReloadIfChangedOnDisk();

    // Grava agora, se houver alteração pendente (usado ao encerrar).
    void FlushNow();

    const std::wstring& Path() const { return path_; }

private:
    void SaveLocked(const Settings& s);
    bool ReadFile(Settings& out) const;
    void WriterLoop();
    uint64_t FileStamp() const;

    mutable std::mutex mu_;
    Settings cur_;
    uint32_t version_ = 1;
    std::wstring path_;
    uint64_t lastStamp_ = 0;

    std::mutex wmu_;
    std::condition_variable wcv_;
    bool dirty_ = false;
    bool stop_ = false;
    uint64_t dirtySince_ = 0;
    std::thread writer_;
};

}  // namespace eoscam

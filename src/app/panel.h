// Ícone na bandeja e painel de controle (só no eoscam-bg.exe). Janela Win32 nativa, sem dependências.
#pragma once

#include <windows.h>

#include <memory>

#include "bridge.h"
#include "settings.h"

namespace eoscam {

// Classe da janela oculta que recebe os cliques da bandeja e as mensagens do "eoscam panel".
inline constexpr wchar_t kBridgeWindowClass[] = L"EosCamBridgeWindow";
// Mensagem que "eoscam panel" envia para a janela oculta: abre (ou traz para frente) o painel.
inline constexpr UINT kMsgOpenPanel = WM_APP + 1;

class PanelApp {
public:
    PanelApp(Bridge& bridge, SettingsStore& store);
    ~PanelApp();
    PanelApp(const PanelApp&) = delete;
    PanelApp& operator=(const PanelApp&) = delete;

    // Cria a janela oculta e o ícone da bandeja e roda o laço de mensagens até sair.
    int Run();
    // De qualquer thread: encerra o laço de mensagens.
    void RequestQuit();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace eoscam

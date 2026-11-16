// Guarda o quadro NV12 que a fonte entrega ao cliente e desenha as telas de aviso (sem quadros,
// versões incompatíveis etc.). A imagem da câmera já chega pronta da ponte (eoscam.exe), em NV12
// 1920x1080: esta classe não decodifica nem escala nada, para a DLL continuar leve dentro do
// Frame Server (que não deve carregar WIC, Maxine nem CUDA).
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace eoscam {

class FrameRenderer {
public:
    HRESULT Init(UINT width, UINT height);
    // Desenha uma tela de aviso (só redesenha se o texto mudou desde a última chamada).
    void RenderMessage(const std::wstring& title, const std::wstring& detail);
    // Chamar depois de escrever um quadro real em Nv12Data(): a próxima tela de aviso será redesenhada.
    void InvalidateMessage() { lastMessage_.clear(); }

    const std::vector<uint8_t>& Nv12() const { return nv12_; }
    uint8_t* Nv12Data() { return nv12_.data(); }
    uint32_t Nv12Bytes() const { return uint32_t(nv12_.size()); }
    UINT Width() const { return w_; }
    UINT Height() const { return h_; }

private:
    UINT w_ = 0, h_ = 0;
    std::vector<uint8_t> nv12_;
    std::wstring lastMessage_;
};

}  // namespace eoscam

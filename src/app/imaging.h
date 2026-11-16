// Núcleos de imagem na CPU: ajustes de imagem (brilho, contraste, gamma, temperatura, tom e saturação),
// conversão para YUV, reamostragem para 1920x1080, composição com o fundo virtual e saída NV12 (BT.601,
// faixa limitada). Depois de convertida para planos Y/U/V na resolução de origem, a imagem é reamostrada
// e composta plano a plano (a mistura alfa é linear, então vale tanto em YUV quanto em RGB): isso evita
// trabalhar com 3 canais na resolução de saída. Faixas de linhas rodam num pequeno pool de threads.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "settings.h"

namespace eoscam {

// Pool simples: ParallelFor(n, fn) chama fn(i) para i em [0, n) nas threads do pool (e na chamadora).
class ThreadPool {
public:
    explicit ThreadPool(int threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    int Size() const { return int(workers_.size()) + 1; }
    void ParallelFor(int n, const std::function<void(int)>& fn);

private:
    void Work();
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_, doneCv_;
    const std::function<void(int)>* fn_ = nullptr;
    std::atomic<int> next_{0};
    int count_ = 0;
    int active_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

// Tabelas de ajuste de imagem (uma LUT de 256 entradas por canal + saturação).
struct AdjustLut {
    uint8_t b[256], g[256], r[256];
    int sat = 128;  // saturação em ponto fixo 1.7 (128 = 100%)
    bool identity = true;
};
void BuildAdjust(const ImageSettings& s, AdjustLut& lut);

// Suaviza levemente a máscara (3x3) para evitar bordas serrilhadas ao ampliá-la.
void FeatherMask(uint8_t* mask, int w, int h, int pitch, std::vector<uint8_t>& tmp);

// Desfoque (caixa, 3 passadas ~ gaussiano) de uma imagem BGR u8 pequena, in-place.
// radius em pixels da imagem.
void BoxBlurBgr(uint8_t* img, int w, int h, int radius, std::vector<uint8_t>& tmp);

// Desfoque "ciente da máscara": só os pixels de fundo (mask baixa) contribuem, então a cor da pessoa não vaza
// para o fundo desfocado (sem halo). mask tem o mesmo tamanho da imagem (255 = pessoa).
void MaskedBoxBlurBgr(uint8_t* img, const uint8_t* mask, int w, int h, int radius);

// Reduz uma imagem BGR u8 por um fator inteiro (média em blocos).
void DownscaleBgr(const uint8_t* src, int sw, int sh, int spitch, int factor, std::vector<uint8_t>& dst, int& dw,
                  int& dh);

// Recorta e redimensiona (modo "cover": preenche 1920x1080 com recorte central) uma imagem BGR u8.
void CoverBgr(const uint8_t* src, int sw, int sh, int spitch, int dw, int dh, std::vector<uint8_t>& dst);

// Imagem em planos Y, U e V (BT.601, faixa limitada); U e V têm metade da resolução em cada direção.
struct YuvImage {
    int w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
};

// BGR u8 -> YuvImage (w e h pares), aplicando os ajustes de imagem (adj pode ser nullptr).
void BgrToYuv(const uint8_t* bgr, int w, int h, int pitch, const AdjustLut* adj, YuvImage& out, ThreadPool& pool);

enum class BgKind { None, Solid, Image, Blur };

struct FinalParams {
    // imagem da câmera (BGR u8), já com os efeitos de IA
    const uint8_t* src = nullptr;
    int sw = 0, sh = 0, spitch = 0;
    // máscara (u8, 255 = pessoa) opcional, com a mesma proporção da imagem
    const uint8_t* mask = nullptr;
    int mw = 0, mh = 0, mpitch = 0;
    const AdjustLut* adj = nullptr;  // nullptr ou identity: sem ajustes
    BgKind bg = BgKind::None;
    uint8_t solidB = 0, solidG = 0, solidR = 0;
    const YuvImage* bgImage = nullptr;  // 1920x1080 (modo Image)
    const uint8_t* plate = nullptr;    // imagem de fundo desfocada, BGR compacto pw x ph (modo Blur)
    int pw = 0, ph = 0;
    // saída NV12 1920x1080
    uint8_t* out = nullptr;
};

constexpr int kOutW = 1920;
constexpr int kOutH = 1080;

void RenderFinal(const FinalParams& p, ThreadPool& pool);

}  // namespace eoscam

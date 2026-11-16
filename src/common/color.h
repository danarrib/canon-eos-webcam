// Conversões BGRA <-> NV12 (BT.601, faixa limitada 16-235), usadas pela fonte de
// vídeo e pela ferramenta de teste.
#pragma once

#include <cstdint>

namespace eoscam {

inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// w e h precisam ser pares. nv12 deve ter w*h*3/2 bytes.
inline void BgraToNv12(const uint8_t* bgra, int stride, int w, int h, uint8_t* nv12) {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + size_t(w) * h;
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r0 = bgra + size_t(y) * stride;
        const uint8_t* r1 = r0 + stride;
        uint8_t* y0 = yPlane + size_t(y) * w;
        uint8_t* y1 = y0 + w;
        uint8_t* uv = uvPlane + size_t(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int sb = 0, sg = 0, sr = 0;
            const uint8_t* px[4] = {r0 + x * 4, r0 + x * 4 + 4, r1 + x * 4, r1 + x * 4 + 4};
            uint8_t* py[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int k = 0; k < 4; k++) {
                int b = px[k][0], g = px[k][1], r = px[k][2];
                *py[k] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                sb += b; sg += g; sr += r;
            }
            sb >>= 2; sg >>= 2; sr >>= 2;
            uv[x] = Clamp8(((-38 * sr - 74 * sg + 112 * sb + 128) >> 8) + 128);
            uv[x + 1] = Clamp8(((112 * sr - 94 * sg - 18 * sb + 128) >> 8) + 128);
        }
    }
}

inline void Nv12ToBgra(const uint8_t* nv12, int w, int h, uint8_t* bgra) {
    const uint8_t* yPlane = nv12;
    const uint8_t* uvPlane = nv12 + size_t(w) * h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int c = yPlane[size_t(y) * w + x] - 16;
            const uint8_t* uv = uvPlane + size_t(y / 2) * w + (x & ~1);
            int d = uv[0] - 128, e = uv[1] - 128;
            uint8_t* o = bgra + (size_t(y) * w + x) * 4;
            o[0] = Clamp8((298 * c + 516 * d + 128) >> 8);
            o[1] = Clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[2] = Clamp8((298 * c + 409 * e + 128) >> 8);
            o[3] = 255;
        }
    }
}

}  // namespace eoscam

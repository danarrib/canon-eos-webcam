#include "jpeg.h"

#include <turbojpeg.h>

namespace canon_eos_webcam {

bool DecodeJpegRgb(const uint8_t* jpeg, size_t size, std::vector<uint8_t>& rgb, int& w, int& h) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj) return false;
    bool ok = false;
    if (tj3DecompressHeader(tj, jpeg, size) == 0) {
        w = tj3Get(tj, TJPARAM_JPEGWIDTH);
        h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
        if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            rgb.resize(size_t(w) * size_t(h) * 3);
            ok = tj3Decompress8(tj, jpeg, size, rgb.data(), w * 3, TJPF_RGB) == 0;
        }
    }
    tj3Destroy(tj);
    return ok;
}

bool EncodeJpegRgb(const uint8_t* rgb, int w, int h, int quality, std::vector<uint8_t>& out) {
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj) return false;
    tj3Set(tj, TJPARAM_QUALITY, quality);
    tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);
    unsigned char* buf = nullptr;
    size_t len = 0;
    bool ok = tj3Compress8(tj, rgb, w, w * 3, h, TJPF_RGB, &buf, &len) == 0;
    if (ok) out.assign(buf, buf + len);
    tj3Free(buf);
    tj3Destroy(tj);
    return ok;
}

}  // namespace canon_eos_webcam

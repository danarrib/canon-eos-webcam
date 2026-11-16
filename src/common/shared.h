// Canal de quadros entre eoscam.exe (sessão do usuário, fala com a câmera) e
// eoscam-source.dll (carregada pelo Frame Server, conta LocalService, sessão 0).
//
// A memória compartilhada fica no namespace Global\. Criar objetos ali exige
// SeCreateGlobalPrivilege, que serviços têm e usuários comuns não: por isso a DLL
// cria e o app apenas abre. O app mantém o handle aberto, então o canal sobrevive
// enquanto o app estiver rodando, mesmo que o Frame Server descarregue a DLL.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace eoscam {

// {D4DFB62F-F0E4-4DD7-A4AA-EDB7F8E013BC}
inline constexpr GUID kSourceClsid = {0xd4dfb62f, 0xf0e4, 0x4dd7, {0xa4, 0xaa, 0xed, 0xb7, 0xf8, 0xe0, 0x13, 0xbc}};
#define EOSCAM_CLSID_STRING L"{D4DFB62F-F0E4-4DD7-A4AA-EDB7F8E013BC}"

inline constexpr wchar_t kCameraName[] = L"Canon EOS Webcam (eoscam)";
// Com a variável de ambiente EOSCAM_LOCAL=1 o canal usa o namespace Local\ (sessão
// atual), o que permite testar a fonte dentro de um processo comum, sem o Frame Server.
inline bool UseLocalNamespace() {
    wchar_t v[8];
    return GetEnvironmentVariableW(L"EOSCAM_LOCAL", v, 8) > 0 && v[0] == L'1';
}
inline const wchar_t* MappingName() {
    return UseLocalNamespace() ? L"Local\\EosCamFrames1" : L"Global\\EosCamFrames1";
}
inline const wchar_t* EventName() {
    return UseLocalNamespace() ? L"Local\\EosCamFrameReady1" : L"Global\\EosCamFrameReady1";
}
inline constexpr wchar_t kDataDir[] = L"C:\\ProgramData\\EosCam";

// SYSTEM, LocalService, Administradores e usuários autenticados têm acesso total.
inline constexpr wchar_t kSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;AU)";

constexpr uint32_t kMagic = 0x4D414345;  // "ECAM"
constexpr uint32_t kLayoutVersion = 1;
constexpr uint32_t kMaxJpeg = 2u << 20;
constexpr int kSlotCount = 3;

// Consumidor considerado ativo se deu sinal de vida há menos que isso.
constexpr uint64_t kConsumerTimeoutMs = 2500;
// Produtor considerado vivo se deu sinal de vida há menos que isso.
constexpr uint64_t kProducerTimeoutMs = 3000;

enum class ProducerState : LONG {
    Unknown = 0,
    Idle = 1,       // app rodando, ninguém usando a webcam
    Starting = 2,   // ligando o Live View
    Streaming = 3,  // enviando quadros
    NoCamera = 4,   // câmera desligada/desconectada/ocupada
};

struct alignas(64) FrameSlot {
    volatile LONG seq;  // seqlock: ímpar enquanto está sendo escrito
    uint32_t size;
    uint64_t frameNumber;
    uint64_t captureTick;
    uint8_t data[kMaxJpeg];
};

struct alignas(64) SharedHeader {
    uint32_t magic;
    uint32_t layoutVersion;
    volatile LONG64 consumerTick;  // GetTickCount64 do último sinal da fonte (quer quadros)
    volatile LONG64 producerTick;  // GetTickCount64 do último sinal do app
    volatile LONG producerState;   // ProducerState
    volatile LONG latestSlot;      // -1 = nenhum quadro ainda
    volatile LONG64 frameNumber;   // incrementa a cada quadro publicado
    volatile LONG consumerWidth;   // informativo: resolução pedida pelo cliente
    volatile LONG consumerHeight;
};

struct SharedFrames {
    SharedHeader header;
    FrameSlot slots[kSlotCount];
};

inline uint64_t NowTick() { return GetTickCount64(); }

class SharedChannel {
public:
    SharedChannel() = default;
    SharedChannel(const SharedChannel&) = delete;
    SharedChannel& operator=(const SharedChannel&) = delete;
    ~SharedChannel() { Close(); }

    // Abre o canal; se não existir e allowCreate, tenta criá-lo.
    // lastError recebe o código do Windows em caso de falha.
    bool Open(bool allowCreate, DWORD* lastError = nullptr) {
        if (frames_) return true;
        HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, MappingName());
        DWORD err = m ? 0 : GetLastError();
        if (!m && allowCreate) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr);
            SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
            ULONGLONG size = sizeof(SharedFrames);
            m = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(size >> 32), DWORD(size),
                                   MappingName());
            err = m ? 0 : GetLastError();
            if (m && !event_) event_ = CreateEventW(&sa, FALSE, FALSE, EventName());
            if (sd) LocalFree(sd);
        }
        if (!m) {
            if (lastError) *lastError = err;
            return false;
        }
        void* v = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedFrames));
        if (!v) {
            if (lastError) *lastError = GetLastError();
            CloseHandle(m);
            return false;
        }
        mapping_ = m;
        frames_ = static_cast<SharedFrames*>(v);
        if (!event_) event_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EventName());

        SharedHeader& h = frames_->header;
        if (h.magic != kMagic || h.layoutVersion != kLayoutVersion) {
            // memória recém-criada (zerada pelo sistema) ou de versão antiga
            std::memset(&h, 0, sizeof h);
            h.latestSlot = -1;
            h.layoutVersion = kLayoutVersion;
            MemoryBarrier();
            h.magic = kMagic;
        }
        return true;
    }

    void Close() {
        if (frames_) UnmapViewOfFile(frames_);
        if (mapping_) CloseHandle(mapping_);
        if (event_) CloseHandle(event_);
        frames_ = nullptr;
        mapping_ = event_ = nullptr;
    }

    bool IsOpen() const { return frames_ != nullptr; }
    SharedHeader& Header() { return frames_->header; }
    HANDLE Event() const { return event_; }

    // ---- lado produtor (app)
    void Publish(const uint8_t* jpeg, size_t n, uint64_t captureTick) {
        if (!frames_ || n == 0 || n > kMaxJpeg) return;
        SharedHeader& h = frames_->header;
        LONG cur = h.latestSlot;
        int slot = (cur < 0 || cur >= kSlotCount) ? 0 : (cur + 1) % kSlotCount;
        FrameSlot& s = frames_->slots[slot];
        InterlockedIncrement(&s.seq);  // ímpar: escrevendo
        std::memcpy(s.data, jpeg, n);
        s.size = uint32_t(n);
        s.frameNumber = uint64_t(h.frameNumber) + 1;
        s.captureTick = captureTick;
        InterlockedIncrement(&s.seq);  // par: pronto
        InterlockedExchange(&h.latestSlot, slot);
        InterlockedIncrement64(&h.frameNumber);
        if (event_) SetEvent(event_);
    }

    void SetProducer(ProducerState st) {
        if (!frames_) return;
        InterlockedExchange(&frames_->header.producerState, LONG(st));
        InterlockedExchange64(&frames_->header.producerTick, LONG64(NowTick()));
    }

    bool ConsumerActive() const {
        if (!frames_) return false;
        uint64_t t = uint64_t(frames_->header.consumerTick);
        return t != 0 && NowTick() - t < kConsumerTimeoutMs;
    }

    // ---- lado consumidor (DLL)
    void ConsumerHeartbeat(LONG w, LONG h) {
        if (!frames_) return;
        InterlockedExchange64(&frames_->header.consumerTick, LONG64(NowTick()));
        InterlockedExchange(&frames_->header.consumerWidth, w);
        InterlockedExchange(&frames_->header.consumerHeight, h);
    }

    uint64_t LatestFrameNumber() const { return frames_ ? uint64_t(frames_->header.frameNumber) : 0; }

    bool ProducerAlive() const {
        if (!frames_) return false;
        uint64_t t = uint64_t(frames_->header.producerTick);
        return t != 0 && NowTick() - t < kProducerTimeoutMs;
    }

    ProducerState Producer() const {
        return frames_ ? ProducerState(frames_->header.producerState) : ProducerState::Unknown;
    }

    bool ReadLatest(std::vector<uint8_t>& out, uint64_t& frameNumber, uint64_t& captureTick) {
        if (!frames_) return false;
        LONG slot = frames_->header.latestSlot;
        if (slot < 0 || slot >= kSlotCount) return false;
        FrameSlot& s = frames_->slots[slot];
        for (int attempt = 0; attempt < 8; attempt++) {
            LONG a = s.seq;
            if (a & 1) { YieldProcessor(); continue; }
            MemoryBarrier();
            uint32_t n = s.size;
            if (n == 0 || n > kMaxJpeg) return false;
            out.assign(s.data, s.data + n);
            frameNumber = s.frameNumber;
            captureTick = s.captureTick;
            MemoryBarrier();
            if (s.seq == a) return true;
        }
        return false;
    }

private:
    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    SharedFrames* frames_ = nullptr;
};

}  // namespace eoscam

// Acesso ao Live View de câmeras Canon EOS via PTP, pelo driver WPD/MTP padrão do Windows.
#pragma once

#include <windows.h>
#include <PortableDeviceApi.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace eoscam {

struct PtpResult {
    HRESULT hr = E_FAIL;
    DWORD code = 0;
    std::vector<DWORD> params;
    std::vector<BYTE> data;
    bool ok() const { return SUCCEEDED(hr) && code == 0x2001; }
};

class PtpDevice {
public:
    HRESULT Open(const std::wstring& id);
    void Close();
    bool IsOpen() const { return dev_ != nullptr; }
    PtpResult Exec(WORD op, std::initializer_list<DWORD> p = {});
    PtpResult Read(WORD op, std::initializer_list<DWORD> p = {});
    PtpResult Write(WORD op, std::initializer_list<DWORD> p, const std::vector<BYTE>& data);

private:
    HRESULT Send(IPortableDeviceValues* in, Microsoft::WRL::ComPtr<IPortableDeviceValues>& out);
    HRESULT EndTransfer(PCWSTR ctx, PtpResult& r);
    Microsoft::WRL::ComPtr<IPortableDevice> dev_;
};

// Códigos de propriedades Canon EOS (PTP). Valores e codificações conferidos no libgphoto2
// (camlibs/ptp2/ptp.h e config.c), usados só como referência.
enum : uint32_t {
    kPropISO = 0xD103,
    kPropExpComp = 0xD104,
    kPropWhiteBalance = 0xD109,
    kPropColorTemp = 0xD10A,
    kPropPictureStyle = 0xD110,
    kPropAlo = 0xD1C1,
};

// Última leitura de uma propriedade da câmera, alimentada pelos eventos PropValueChanged (0xC189) e
// AvailListChanged (0xC18A) do GetEvent (0x9116).
struct PropState {
    bool hasValue = false;
    uint32_t value = 0;
    bool hasList = false;
    std::vector<uint32_t> list;  // valores permitidos anunciados pela câmera
    std::vector<uint8_t> raw;    // bytes do último PropValueChanged (para propriedades estruturadas)
};

class CanonCamera {
public:
    enum class Frame { Ok, NotReady, Error };

    ~CanonCamera() { Disconnect(); }

    // Procura a primeira câmera Canon, abre e liga o Live View para o PC.
    bool Connect(std::string& error);
    // Desliga o Live View, sai do modo remoto e fecha o dispositivo.
    void Disconnect();
    bool IsConnected() const { return connected_; }
    const std::string& Name() const { return name_; }
    // Diagnóstico: última resposta de GetViewFinderData que não foi um quadro.
    HRESULT LastHr() const { return lastHr_; }
    DWORD LastCode() const { return lastCode_; }
    size_t LastSize() const { return lastSize_; }
    // Quantas vezes o Live View teve de ser religado porque a câmera parou de mandar quadros
    // (acontece quando outro software, como o driver do EOS Webcam Utility, mexe na câmera).
    int Recoveries() const { return recoveries_; }

    // Busca o quadro mais recente do Live View (JPEG).
    Frame GetFrame(std::vector<uint8_t>& jpeg);

    // ---- propriedades da câmera (ajustes do painel)
    // Intervalo de leitura dos eventos (ms); o painel aberto pede um intervalo menor.
    void SetEventInterval(DWORD ms) { eventIntervalMs_ = ms; }
    // Esvazia a fila de eventos da câmera e atualiza o cache. Devolve true se algo mudou.
    bool PollEvents();
    // Cópia do estado de uma propriedade (false se a câmera nunca a anunciou). Thread-safe.
    bool GetProp(uint32_t prop, PropState& out) const;
    // Cópia de todas as propriedades conhecidas (diagnóstico: "eoscam cam").
    std::map<uint32_t, PropState> AllProps() const {
        std::lock_guard<std::mutex> lk(propsMu_);
        return props_;
    }
    // Quantas vezes o cache mudou (o painel só redesenha quando muda).
    uint32_t PropsGeneration() const { return propsGen_; }
    // Escreve uma propriedade (SetDevicePropValueEx). 0x2001 = aceito; outros = a câmera recusou.
    PtpResult WriteProp(uint32_t prop, uint32_t value);
    // Escreve uma propriedade estruturada (lista de palavras de 32 bits).
    PtpResult WritePropWords(uint32_t prop, const std::vector<uint32_t>& words);

private:
    PtpResult SetPropEx(uint32_t prop, uint32_t value);
    bool EnableLiveView();
    void ParseEvents(const std::vector<BYTE>& d);

    PtpDevice dev_;
    bool connected_ = false;
    std::string name_;
    uint64_t lastEventTick_ = 0;
    int consecutiveErrors_ = 0;
    HRESULT lastHr_ = S_OK;
    DWORD lastCode_ = 0;
    size_t lastSize_ = 0;
    uint64_t lastOkTick_ = 0;
    uint64_t lastRecoveryTick_ = 0;
    uint64_t connectTick_ = 0;
    int recoveries_ = 0;
    int recoveriesSinceOk_ = 0;

    DWORD eventIntervalMs_ = 1000;
    mutable std::mutex propsMu_;
    std::map<uint32_t, PropState> props_;
    volatile uint32_t propsGen_ = 0;
    bool dumpEvents_ = false;
};

std::string W2U(const wchar_t* w);

}  // namespace eoscam

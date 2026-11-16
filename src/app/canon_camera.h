// Acesso ao Live View de câmeras Canon EOS via PTP, pelo driver WPD/MTP padrão do Windows.
#pragma once

#include <windows.h>
#include <PortableDeviceApi.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
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

private:
    PtpResult SetPropEx(uint32_t prop, uint32_t value);
    bool EnableLiveView();

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
};

std::string W2U(const wchar_t* w);

}  // namespace eoscam

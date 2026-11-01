#pragma once

#include <d3d11.h>
#include <mfmediaengine.h>
#include <mfapi.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include "imgui.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

class VideoPlayer : public IMFMediaEngineNotify
{
public:
    VideoPlayer() : 
        m_refCount(1), 
        m_videoWidth(0), 
        m_videoHeight(0), 
        m_isPlaying(false), 
        m_isReady(false),
        m_pd3dDevice(nullptr),
        m_pVideoTexture(nullptr),
        m_pVideoSRV(nullptr),
        m_mfStarted(false)
    {
    }

    ~VideoPlayer();

    // Initialize the Media Foundation Subsystem and DXGI Device Manager
    bool Initialize(ID3D11Device* device);
    void DrawFullscreen();
    // Load an MP4 file resource path
    bool Open(const wchar_t* videoPath, bool loop, bool autoPlay);

    // Tick function to update the D3D11 texture
    void Update();

    // Render control widgets inside an ImGui Window canvas context
    void DrawImGuiWindow(const char* windowName);

    // Playback Wrapper Functions
    void Play();
    void Pause();
    void Skip();
    void Seek(double time);
    void SetVolume(double vol);
    double GetCurrentTime();
    double GetDuration();
    bool IsReady() const;
    bool IsPlaying() const;
    bool IsFinished() const;
    // Shutdown and release memory resources
    void Shutdown();

    // COM Implementation Requirements for IMFMediaEngineNotify
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    
    STDMETHODIMP EventNotify(DWORD Event, DWORD_PTR Param1, DWORD Param2) override;

private:
    void ReleaseTextureResources();

    bool Fail();
    bool CreateTextureResources(DWORD width, DWORD height);
    volatile LONG m_refCount = 1;
    int m_videoWidth = 0;
    int m_videoHeight = 0;

    // Written from the MF worker thread, read from the render thread
    std::atomic<bool> m_isReady{ false };
    std::atomic<bool> m_isPlaying{ false };
    std::atomic<bool> m_isFinished{ false };
    std::atomic<bool> m_hasError{ false };
    std::atomic<bool> m_loop{ false };
    std::atomic<bool> m_autoPlay{ true };

    bool m_mfStarted = false;

    ID3D11Device* m_pd3dDevice = nullptr;
    ComPtr<IMFDXGIDeviceManager> m_pDXGIManager;
    ComPtr<IMFMediaEngine> m_pMediaEngine;
    ComPtr<ID3D11Texture2D> m_pVideoTexture;
    ComPtr<ID3D11ShaderResourceView> m_pVideoSRV;
};

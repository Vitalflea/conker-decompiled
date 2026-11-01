#include "VideoPlayer.hpp"

#include <mfapi.h>
#include "imgui.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

VideoPlayer::~VideoPlayer()
{
    Shutdown();
}

bool VideoPlayer::Initialize(ID3D11Device* device)
{
    if (m_pMediaEngine) return true; // already initialized
    m_pd3dDevice = device;

    if (FAILED(MFStartup(MF_VERSION)))
        return false;
    m_mfStarted = true;

    // The media engine uses the device from its own threads
    ComPtr<ID3D10Multithread> pMultithread;
    if (SUCCEEDED(m_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pMultithread))))
        pMultithread->SetMultithreadProtected(TRUE);

    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &m_pDXGIManager)))
        return Fail();
    if (FAILED(m_pDXGIManager->ResetDevice(m_pd3dDevice, resetToken)))
        return Fail();

    ComPtr<IMFAttributes> pAttributes;
    if (FAILED(MFCreateAttributes(&pAttributes, 3)))
        return Fail();
    if (FAILED(pAttributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, m_pDXGIManager.Get())))
        return Fail();
    if (FAILED(pAttributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, static_cast<IMFMediaEngineNotify*>(this))))
        return Fail();
    if (FAILED(pAttributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM)))
        return Fail();

    ComPtr<IMFMediaEngineClassFactory> pFactory;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))))
        return Fail();
    if (FAILED(pFactory->CreateInstance(0, pAttributes.Get(), &m_pMediaEngine)))
        return Fail();

    return true;
}

bool VideoPlayer::Open(const wchar_t* videoPath, bool loop, bool autoPlay)
{
    if (!m_pMediaEngine) return false;

    m_isReady = false;
    m_isPlaying = false;
    m_isFinished = false;
    m_hasError = false;
    m_loop = loop;
    m_autoPlay = autoPlay;

    wchar_t fullPath[MAX_PATH] = {};
    DWORD len = GetFullPathNameW(videoPath, MAX_PATH, fullPath, nullptr);
    if (len == 0 || len >= MAX_PATH)
        return false;

    BSTR bstrURL = SysAllocString(fullPath);
    if (!bstrURL) return false;
    HRESULT hr = m_pMediaEngine->SetSource(bstrURL);
    SysFreeString(bstrURL);

    if (FAILED(hr)) { m_hasError = true; return false; }
    return true;
}

void VideoPlayer::Update()
{
    if (!m_pMediaEngine || !m_isReady || m_hasError) return;

    LONGLONG pts = 0;
    if (m_pMediaEngine->OnVideoStreamTick(&pts) != S_OK)
        return; // no new frame yet

    DWORD width = 0, height = 0;
    if (FAILED(m_pMediaEngine->GetNativeVideoSize(&width, &height)) || width == 0 || height == 0)
        return;

    if (!m_pVideoTexture || m_videoWidth != (int)width || m_videoHeight != (int)height)
    {
        if (!CreateTextureResources(width, height))
            return;
    }

    MFVideoNormalizedRect src = { 0.0f, 0.0f, 1.0f, 1.0f };
    RECT dst = { 0, 0, m_videoWidth, m_videoHeight };
    m_pMediaEngine->TransferVideoFrame(m_pVideoTexture.Get(), &src, &dst, nullptr);
}

void VideoPlayer::DrawFullscreen()
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;

    dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 255));
    if (!m_pVideoSRV || m_videoWidth == 0 || m_videoHeight == 0) return;

    float scale = (screen.x / m_videoWidth < screen.y / m_videoHeight)
                      ? screen.x / m_videoWidth
                      : screen.y / m_videoHeight;
    ImVec2 size(m_videoWidth * scale, m_videoHeight * scale);
    ImVec2 p0((screen.x - size.x) * 0.5f, (screen.y - size.y) * 0.5f);
    ImVec2 p1(p0.x + size.x, p0.y + size.y);

    dl->AddImage((ImTextureID)(intptr_t)m_pVideoSRV.Get(), p0, p1);
}

void VideoPlayer::DrawImGuiWindow(const char* windowName)
{
    ImGui::Begin(windowName);

    if (m_hasError)
    {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Video failed to load or decode.");
        ImGui::End();
        return;
    }
    if (!m_isReady)
    {
        ImGui::Text("Loading...");
        ImGui::End();
        return;
    }

    if (m_pVideoSRV && m_videoWidth > 0)
    {
        float w = ImGui::GetContentRegionAvail().x;
        float aspect = (float)m_videoHeight / (float)m_videoWidth;
        ImGui::Image((ImTextureID)(intptr_t)m_pVideoSRV.Get(), ImVec2(w, w * aspect));
    }
    else
    {
        ImGui::Text("No frame yet.");
    }

    ImGui::Separator();

    if (m_isPlaying) { if (ImGui::Button("Pause")) Pause(); }
    else             { if (ImGui::Button("Play"))  Play();  }

    ImGui::SameLine();
    if (ImGui::Button("Rewind")) { Seek(0.0); m_isFinished = false; }

    double currentTime = GetCurrentTime();
    double duration = GetDuration();
    if (!(duration > 0.0)) duration = 1.0; // also catches NaN/inf for unknown duration

    float seekTime = (float)currentTime;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-150.0f);
    if (ImGui::SliderFloat("##Timeline", &seekTime, 0.0f, (float)duration, "%.1fs"))
        Seek((double)seekTime);

    ImGui::SameLine();
    ImGui::Text("%.1f / %.1f", currentTime, duration);

    static float vol = 1.0f;
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f, "%.2f"))
        SetVolume((double)vol);

    ImGui::End();
}

void VideoPlayer::Play()
{
    if (m_pMediaEngine && SUCCEEDED(m_pMediaEngine->Play())) m_isPlaying = true;
}

void VideoPlayer::Pause()
{
    if (m_pMediaEngine && SUCCEEDED(m_pMediaEngine->Pause())) m_isPlaying = false;
}

void VideoPlayer::Seek(double t)
{
    if (m_pMediaEngine) m_pMediaEngine->SetCurrentTime(t);
}

void VideoPlayer::SetVolume(double v)
{
    if (m_pMediaEngine) m_pMediaEngine->SetVolume(v);
}

void VideoPlayer::Skip()
{
    if (m_pMediaEngine) m_pMediaEngine->Pause();
    m_isPlaying = false;
    m_isFinished = true;
}

double VideoPlayer::GetCurrentTime()
{
    return m_pMediaEngine ? m_pMediaEngine->GetCurrentTime() : 0.0;
}

double VideoPlayer::GetDuration()
{
    return m_pMediaEngine ? m_pMediaEngine->GetDuration() : 0.0;
}

void VideoPlayer::Shutdown()
{
    // Engine first: it may still be writing to the texture
    if (m_pMediaEngine)
    {
        m_pMediaEngine->Shutdown();
        m_pMediaEngine.Reset();
    }
    ReleaseTextureResources();
    m_pDXGIManager.Reset();
    m_pd3dDevice = nullptr;

    if (m_mfStarted)
    {
        MFShutdown();
        m_mfStarted = false;
    }
}

// ---- IMFMediaEngineNotify ----

STDMETHODIMP VideoPlayer::QueryInterface(REFIID riid, void** ppvObject)
{
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify))
    {
        *ppvObject = static_cast<IMFMediaEngineNotify*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) VideoPlayer::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) VideoPlayer::Release()
{
    return InterlockedDecrement(&m_refCount); // lifetime owned externally
}

STDMETHODIMP VideoPlayer::EventNotify(DWORD event, DWORD_PTR /*param1*/, DWORD /*param2*/)
{
    switch (event)
    {
    case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        m_isReady = true;
        if (m_autoPlay) Play();
        break;

    case MF_MEDIA_ENGINE_EVENT_ENDED:
        if (m_loop)
        {
            Seek(0.0);
            Play();
        }
        else
        {
            m_isPlaying = false;
            m_isFinished = true;
        }
        break;

    case MF_MEDIA_ENGINE_EVENT_ERROR:
        m_hasError = true;
        m_isPlaying = false;
        m_isFinished = true; // so the game doesn't wait forever
        break;
    }
    return S_OK;
}

// ---- private ----

bool VideoPlayer::IsReady() const { return m_isReady; }
bool VideoPlayer::IsPlaying() const { return m_isPlaying; }
bool VideoPlayer::IsFinished() const { return m_isFinished; }

bool VideoPlayer::Fail()
{
    Shutdown();
    return false;
}

bool VideoPlayer::CreateTextureResources(DWORD width, DWORD height)
{
    ReleaseTextureResources();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    if (FAILED(m_pd3dDevice->CreateTexture2D(&desc, nullptr, &m_pVideoTexture)) ||
        FAILED(m_pd3dDevice->CreateShaderResourceView(m_pVideoTexture.Get(), nullptr, &m_pVideoSRV)))
    {
        ReleaseTextureResources();
        return false;
    }

    m_videoWidth = (int)width;
    m_videoHeight = (int)height;
    return true;
}

void VideoPlayer::ReleaseTextureResources()
{
    m_pVideoSRV.Reset();
    m_pVideoTexture.Reset();
    m_videoWidth = 0;
    m_videoHeight = 0;
}

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

// Dear ImGui: standalone example application for Windows API + DirectX 11 + Native Video Playback
#include <Windows.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

// Windows Media Foundation Headers
#include <mfmediaengine.h>
#include <mfapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Media Foundation State Variables
static ComPtr<IMFMediaEngine>       g_pMediaEngine = nullptr;
static ID3D11Texture2D* g_pVideoTexture = nullptr;
static ID3D11ShaderResourceView* g_pVideoSRV = nullptr;
static bool                         g_VideoPlaying = false;
static int                          g_VideoWidth = 640;
static int                          g_VideoHeight = 360;

// Dummy callback class required to initialize IMFMediaEngine
class MediaEngineNotify : public IMFMediaEngineNotify {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppvObject = this; AddRef(); return S_OK;
        }
        *ppvObject = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return 1; }
    STDMETHODIMP_(ULONG) Release() { return 1; }
    STDMETHODIMP EventNotify(DWORD Event, DWORD_PTR Param1, DWORD Param2) {
        if (Event == MF_MEDIA_ENGINE_EVENT_CANPLAY) {
            g_VideoPlaying = true;
            if (g_pMediaEngine) {
                g_pMediaEngine->Play();
            }
        }
        else if (Event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            // Param1 contains the error type, Param2 contains the exact HRESULT error code
            char errorLog[256];
            std::snprintf(errorLog, sizeof(errorLog), "[Media Engine Error] Type: %lld, HRESULT: 0x%lX\n", (long long)Param1, (long)Param2);
            OutputDebugStringA(errorLog); // Prints directly into the Visual Studio Output Window
        }
        return S_OK;
    }
};
static MediaEngineNotify g_MediaEngineNotify;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Native Video Initialization Functions
bool InitMediaEngine(ID3D11Device* device, const wchar_t* videoPath)
{
    // Initialize MF platform subsystem
    if (FAILED(MFStartup(MF_VERSION))) return false;

    // Direct3D Manager generation for the playback device context
    ComPtr<IMFDXGIDeviceManager> pDXGIManager;
    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &pDXGIManager))) return false;
    if (FAILED(pDXGIManager->ResetDevice(device, resetToken))) return false;

    // Attributes setup
    ComPtr<IMFAttributes> pAttributes;
    if (FAILED(MFCreateAttributes(&pAttributes, 4))) return false;
    if (FAILED(pAttributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, pDXGIManager.Get()))) return false;
    if (FAILED(pAttributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, &g_MediaEngineNotify))) return false;
    if (FAILED(pAttributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM))) return false;

    // Factory creation
    ComPtr<IMFMediaEngineClassFactory> pFactory;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) return false;
    if (FAILED(pFactory->CreateInstance(0, pAttributes.Get(), &g_pMediaEngine))) return false;

    // Load file resource URL 
    BSTR bstrURL = SysAllocString(videoPath);
    auto res = g_pMediaEngine->SetSource(bstrURL);
    SysFreeString(bstrURL);

    // Command play configuration
    return true;
}

void UpdateVideoFrame()
{
    if (!g_pMediaEngine) return;

    LONGLONG pts;
    auto res = g_pMediaEngine->OnVideoStreamTick(&pts);
    if (res == S_OK)
    {
        DWORD width = 0, height = 0;
        g_pMediaEngine->GetNativeVideoSize(&width, &height);
        if (width == 0 || height == 0) return;

        // Lazy initialize matching D3D resources if canvas dimensions drift or update
        if (!g_pVideoTexture || g_VideoWidth != width || g_VideoHeight != height)
        {
            g_VideoWidth = width;
            g_VideoHeight = height;

            if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }
            if (g_pVideoSRV) { g_pVideoSRV->Release(); g_pVideoSRV = nullptr; }

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

            g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_pVideoTexture);
            g_pd3dDevice->CreateShaderResourceView(g_pVideoTexture, nullptr, &g_pVideoSRV);
        }

        // Blit engine decoder surfaces directly into your active DX11 texture resource target
        MFVideoNormalizedRect rect = { 0.0f, 0.0f, 1.0f, 1.0f };
        RECT destRect = { 0, 0, g_VideoWidth, g_VideoHeight };
        g_pMediaEngine->TransferVideoFrame(g_pVideoTexture, &rect, &destRect, nullptr);
    }
}

void ShutdownMediaEngine()
{
    if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }
    if (g_pVideoSRV) { g_pVideoSRV->Release(); g_pVideoSRV = nullptr; }
    if (g_pMediaEngine) { g_pMediaEngine->Shutdown(); g_pMediaEngine = nullptr; }
    MFShutdown();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow)
{
    // Initialize COM wrapper requirements for Media Foundation
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX11 Video Example", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ID3D10Multithread* pMultithread = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(__uuidof(ID3D10Multithread), (void**)&pMultithread)))
    {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(main_scale);
    ImGui::GetStyle().FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Start video playback (Replace payload string with your converted video path)
    InitMediaEngine(g_pd3dDevice, L"C:\\Users\\Administrator\\source\\repos\\conker-decompiled\\conker-decompiled\\BootIntro.mp4");

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) {
            break;
        }

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Fetch newly unrolled video texture payloads from Media Foundation
        if (g_VideoPlaying) {

            UpdateVideoFrame();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Display the Native Video Window
        {
            ImGui::Begin("Native MP4 Playback Layer");

            if (g_pVideoSRV)
            {
                // Pull texture width/height and feed straight to ImGui image element
                ImGui::Image((ImTextureID)g_pVideoSRV, ImVec2((float)g_VideoWidth, (float)g_VideoHeight));
            }
            else
            {
                ImGui::Text("Opening video resource stream...");
            }

            ImGui::End();
        }

        // Clear and render backends
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.45f * 1.0f, 0.55f * 1.0f, 0.60f * 1.0f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    // Pipeline destruction sequences
    ShutdownMediaEngine();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();

    return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    // This is a basic setup. Optimally could use e.g. DXGI_SWAP_EFFECT_FLIP_DISCARD and handle fullscreen mode differently. See #8979 for suggestions.
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void* GameAlloc(unsigned size, const char* file, int line, unsigned align)
{
    if (align == (unsigned)-1) align = size < 32 ? 4 : 16;
    if (align < sizeof(void*)) align = sizeof(void*);
    void* p = _aligned_malloc(size ? size : 1, align);
    if (!p) __debugbreak();
    return p;
}
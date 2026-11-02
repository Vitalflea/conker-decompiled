#define NOMINMAX
#pragma once
// Free-fly D3D11 viewer for decoded scenegraph models. Several scenegraphs can be shown together
// (e.g. Conker = 4 plus his tail = 47), and the bind pose skeleton can be overlaid.
// Controls: hold RIGHT MOUSE to look, W/A/S/D to move, Q/E down/up, Shift = fast, mouse wheel = speed.
#include "Package.hpp"
#include "SceneGraph.hpp"
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>

class ModelViewer
{
public:
    bool Init(ID3D11Device* device, Package* package);

    // append = false replaces everything in the scene, append = true adds to it.
    bool LoadModel(u32 scenegraphIndex, bool append = false, std::string* err = nullptr);

    void Update(float dt);      // camera input; call after ImGui::NewFrame()
    void DrawUI();              // ImGui window with models, camera and per-draw controls
    void Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);   // after clearing the back buffer, before ImGui

private:
    struct GpuBuffer
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
    };
    struct GpuSubmesh
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> ib;
        u32 indexCount = 0;
        u32 bufferIndex = 0;                // index into m_buffers (global)
        bool visible = true;
        int texSlot = -1;                   // index into m_texSrv (global), -1 = untextured
        std::string label;
    };
    struct TexSlot
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;   // layer 0 as a plain Texture2D
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int log2w = 0, log2h = 0;
        std::string name;
    };

    void ResetCamera();
    void EnsureDepth(UINT w, UINT h);
    void RebuildSkeleton();

    ID3D11Device* m_dev = nullptr;
    Package* m_pkg = nullptr;
    bool m_ready = false;
    std::string m_status;

    // pipeline
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbVS, m_cbPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster[2][2];       // [wireframe][cull back]
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState, m_depthNoTest;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTex;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    UINT m_depthW = 0, m_depthH = 0;

    // scene
    std::vector<GpuBuffer> m_buffers;
    std::vector<GpuSubmesh> m_subs;
    std::vector<TexSlot> m_tex;
    std::vector<std::string> m_models;      // description of each loaded scenegraph
    std::vector<std::string> m_notes;
    std::vector<ModelBone> m_bones;         // skeleton of the first loaded model
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneVB;
    UINT m_boneVertexCount = 0;
    float m_bmin[3] = { 0, 0, 0 }, m_bmax[3] = { 0, 0, 0 };
    bool m_haveBounds = false;
    int m_sgIndex = 4;

    // camera + options
    DirectX::XMFLOAT3 m_pos = { 0, 60, 250 };
    float m_yaw = 3.14159265f, m_pitch = 0.0f;
    float m_speed = 100.0f;
    bool m_looking = false;
    bool m_wire = false;
    bool m_cullBack = false;
    bool m_showBones = false;
    bool m_lighting = true;
};
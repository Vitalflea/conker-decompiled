#include "ModelViewer.hpp"
#include "TextureLoader.hpp"
#include "imgui.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static const char* kShaderSource = R"HLSL(
cbuffer VSConstants : register(b0)
{
    float4x4 gWorldViewProj;
};
cbuffer PSConstants : register(b1)
{
    float4 gTint;
    float4 gLightDir;       // direction TOWARDS the light (xyz)
    float gHasTexture;
    float gLighting;
    float2 gPad;
};
Texture2D gTex : register(t0);
SamplerState gSmp : register(s0);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 nrm : NORMAL; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float3 nrm : TEXCOORD1; };

VSOut VS(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), gWorldViewProj);
    o.uv = i.uv;
    o.nrm = i.nrm;      // model space, the model matrix is the identity
    return o;
}

float4 PS(VSOut i) : SV_Target
{
    float3 c = gTint.rgb;
    if (gHasTexture > 0.5)
        c = gTex.Sample(gSmp, i.uv).rgb;

    float len = length(i.nrm);
    if (gLighting > 0.5 && len > 0.5)
    {
        float ndl = dot(i.nrm / len, gLightDir.xyz);
        c *= 0.35 + 0.65 * saturate(ndl);
    }
    return float4(c, 1.0);
}
)HLSL";

struct VSConstants { XMFLOAT4X4 wvp; };
struct PSConstants { float tint[4]; float lightDir[4]; float hasTexture; float lighting; float pad[2]; };

static int Log2i(UINT v) { int r = 0; while (v > 1) { v >>= 1; ++r; } return r; }

bool ModelViewer::Init(ID3D11Device* device, Package* package)
{
    m_dev = device;
    m_pkg = package;

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "viewer", nullptr, nullptr,
        "VS", "vs_4_0", 0, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr))
    {
        m_status = errBlob ? std::string((const char*)errBlob->GetBufferPointer()) : "vertex shader compile failed";
        return false;
    }
    hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "viewer", nullptr, nullptr,
        "PS", "ps_4_0", 0, 0, psBlob.GetAddressOf(), errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        m_status = errBlob ? std::string((const char*)errBlob->GetBufferPointer()) : "pixel shader compile failed";
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf())) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf())))
    {
        m_status = "CreateVertexShader/CreatePixelShader failed";
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC il[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(il, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_layout.GetAddressOf())))
    {
        m_status = "CreateInputLayout failed";
        return false;
    }

    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(VSConstants);
    device->CreateBuffer(&cb, nullptr, m_cbVS.GetAddressOf());
    cb.ByteWidth = sizeof(PSConstants);
    device->CreateBuffer(&cb, nullptr, m_cbPS.GetAddressOf());

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    device->CreateSamplerState(&sd, m_sampler.GetAddressOf());

    for (int wire = 0; wire < 2; ++wire)
        for (int cull = 0; cull < 2; ++cull)
        {
            D3D11_RASTERIZER_DESC rd = {};
            rd.FillMode = wire ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
            rd.CullMode = cull ? D3D11_CULL_BACK : D3D11_CULL_NONE;
            rd.FrontCounterClockwise = FALSE;
            rd.DepthClipEnable = TRUE;
            device->CreateRasterizerState(&rd, m_raster[wire][cull].GetAddressOf());
        }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dd, m_depthState.GetAddressOf());
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dd, m_depthNoTest.GetAddressOf());

    m_ready = true;
    m_status = "ready";
    return true;
}

void ModelViewer::EnsureDepth(UINT w, UINT h)
{
    if (m_depthTex && m_depthW == w && m_depthH == h) return;

    m_dsv.Reset();
    m_depthTex.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(m_dev->CreateTexture2D(&td, nullptr, m_depthTex.GetAddressOf())))
        m_dev->CreateDepthStencilView(m_depthTex.Get(), nullptr, m_dsv.GetAddressOf());
    m_depthW = w;
    m_depthH = h;
}

void ModelViewer::ResetCamera()
{
    if (!m_haveBounds) return;
    const float cx = 0.5f * (m_bmin[0] + m_bmax[0]);
    const float cy = 0.5f * (m_bmin[1] + m_bmax[1]);
    const float ex = m_bmax[0] - m_bmin[0];
    const float ey = m_bmax[1] - m_bmin[1];
    const float ez = m_bmax[2] - m_bmin[2];
    const float radius = std::max(1.0f, std::max(ex, std::max(ey, ez)));

    // Conker's nose points to +z, so look at the front from +z toward -z (yaw = pi)
    m_pos = XMFLOAT3(cx, cy, m_bmax[2] + radius * 1.2f);
    m_yaw = 3.14159265f;
    m_pitch = 0.0f;
    m_speed = radius;
}

void ModelViewer::RebuildSkeleton()
{
    m_boneVB.Reset();
    m_boneVertexCount = 0;

    std::vector<ModelVertex> lines;
    for (const ModelBone& b : m_bones)
    {
        if (b.parent < 0 || b.parent >= (int)m_bones.size()) continue;
        const ModelBone& p = m_bones[b.parent];
        lines.push_back({ p.world[0], p.world[1], p.world[2], 0, 0, 0, 0, 0 });
        lines.push_back({ b.world[0], b.world[1], b.world[2], 0, 0, 0, 0, 0 });
    }
    if (lines.empty()) return;

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(lines.size() * sizeof(ModelVertex));
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = lines.data();
    if (SUCCEEDED(m_dev->CreateBuffer(&bd, &sd, m_boneVB.GetAddressOf())))
        m_boneVertexCount = (UINT)lines.size();
}

bool ModelViewer::LoadModel(u32 scenegraphIndex, bool append, std::string* err)
{
    auto fail = [&](const std::string& m) { m_status = m; if (err) *err = m; return false; };

    if (!m_ready || !m_pkg) return fail("viewer not initialised");
    const Asset* a = m_pkg->findByIndex(scenegraphIndex);
    if (!a) return fail("no asset with that index");

    ModelData md;
    std::string e;
    if (!LoadModelData(*m_pkg, *a, md, &e)) return fail(e);

    if (!append)
    {
        m_buffers.clear();
        m_subs.clear();
        m_tex.clear();
        m_models.clear();
        m_notes.clear();
        m_bones.clear();
        m_haveBounds = false;
    }

    const u32 bufBase = (u32)m_buffers.size();
    const int texBase = (int)m_tex.size();

    // ---- vertex buffers
    std::vector<GpuBuffer> buffers(md.buffers.size());
    for (size_t i = 0; i < md.buffers.size(); ++i)
    {
        const ModelBuffer& mb = md.buffers[i];
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.ByteWidth = (UINT)(mb.vertices.size() * sizeof(ModelVertex));
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = mb.vertices.data();
        if (FAILED(m_dev->CreateBuffer(&bd, &sd, buffers[i].vb.GetAddressOf()))) return fail("vertex buffer creation failed");
    }

    // ---- index buffers
    std::vector<GpuSubmesh> subs(md.submeshes.size());
    for (size_t i = 0; i < md.submeshes.size(); ++i)
    {
        const ModelSubmesh& sm = md.submeshes[i];
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.ByteWidth = (UINT)(sm.indices.size() * sizeof(u32));
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = sm.indices.data();
        if (FAILED(m_dev->CreateBuffer(&bd, &sd, subs[i].ib.GetAddressOf()))) return fail("index buffer creation failed");
        subs[i].indexCount = (UINT)sm.indices.size();
        subs[i].bufferIndex = bufBase + sm.bufferIndex;
        subs[i].label = "sg" + std::to_string(scenegraphIndex) + ": " + sm.label;
    }

    // ---- textures: one slot per "texturefileN" entry, deduplicated by asset within this model
    std::vector<TexSlot> newTex(md.textureNames.size());
    std::map<u32, size_t> firstSlotOfAsset;
    for (size_t i = 0; i < md.textureNames.size(); ++i)
    {
        newTex[i].name = "sg" + std::to_string(scenegraphIndex) + " #" + std::to_string(i) + ": " + md.textureNames[i];
        const u32 ti = md.textureAssets[i];
        if (!ti) continue;

        auto it = firstSlotOfAsset.find(ti);
        if (it != firstSlotOfAsset.end())
        {
            const TexSlot& src = newTex[it->second];
            newTex[i].srv = src.srv;
            newTex[i].format = src.format;
            newTex[i].log2w = src.log2w;
            newTex[i].log2h = src.log2h;
            continue;
        }

        const Asset* ta = m_pkg->findByIndex(ti);
        GpuTexture gt;
        if (!ta || !CreateTexture(m_dev, *m_pkg, *ta, gt, nullptr)) continue;

        // layer 0 as a plain Texture2D view (arrays and cube maps would not match the shader's Texture2D)
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = gt.format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = 0;
        sv.Texture2D.MipLevels = gt.mipLevels;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (SUCCEEDED(m_dev->CreateShaderResourceView(gt.texture.Get(), &sv, srv.GetAddressOf())))
        {
            newTex[i].srv = srv;
            newTex[i].format = gt.format;
            newTex[i].log2w = Log2i(gt.width);
            newTex[i].log2h = Log2i(gt.height);
            firstSlotOfAsset[ti] = i;
        }
    }
    for (TexSlot& t : newTex) m_tex.push_back(t);

    // ---- default texture per draw: the push buffer's stage 1 format (colour format + size) is matched against
    //      every loaded texture. This is a guess that matched the real choices for Conker, see notes.
    int defaultSlot = -1;
    for (size_t i = texBase; i < m_tex.size(); ++i)
        if (m_tex[i].srv) { defaultSlot = (int)i; break; }

    for (size_t i = 0; i < subs.size(); ++i)
    {
        subs[i].texSlot = defaultSlot;
        const ModelSubmesh& sm = md.submeshes[i];
        if (!sm.hasStage1) continue;

        const u32 nvFmt = (sm.stage1Format >> 8) & 0xFF;
        const int wantW = (int)((sm.stage1Format >> 20) & 0xF), wantH = (int)((sm.stage1Format >> 24) & 0xF);
        const DXGI_FORMAT want = nvFmt == 0x0C ? DXGI_FORMAT_BC1_UNORM
            : nvFmt == 0x0E ? DXGI_FORMAT_BC2_UNORM
            : nvFmt == 0x0F ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_UNKNOWN;
        if (want == DXGI_FORMAT_UNKNOWN) continue;

        int best = -1, bestScore = 1 << 30;
        for (size_t t = 0; t < m_tex.size(); ++t)
        {
            if (!m_tex[t].srv || m_tex[t].format != want) continue;
            const int score = std::abs(m_tex[t].log2w - wantW) + std::abs(m_tex[t].log2h - wantH);
            if (score < bestScore) { bestScore = score; best = (int)t; }
        }
        if (best >= 0) subs[i].texSlot = best;
    }

    for (GpuBuffer& b : buffers) m_buffers.push_back(std::move(b));
    for (GpuSubmesh& s : subs) m_subs.push_back(std::move(s));

    // bounds, skeleton, notes
    for (int k = 0; k < 3; ++k)
    {
        m_bmin[k] = m_haveBounds ? std::min(m_bmin[k], md.boundsMin[k]) : md.boundsMin[k];
        m_bmax[k] = m_haveBounds ? std::max(m_bmax[k], md.boundsMax[k]) : md.boundsMax[k];
    }
    m_haveBounds = true;

    if (m_bones.empty() && !md.bones.empty())
    {
        m_bones = md.bones;
        RebuildSkeleton();
    }
    for (const std::string& n : md.notes) m_notes.push_back("sg" + std::to_string(scenegraphIndex) + ": " + n);

    m_models.push_back("scenegraph " + std::to_string(scenegraphIndex) + ": " + std::to_string(md.submeshes.size()) +
        " draws, " + std::to_string(md.buffers.size()) + " vertex buffers, " +
        std::to_string(md.bones.size()) + " bones");
    m_sgIndex = (int)scenegraphIndex;
    if (!append) ResetCamera();

    m_status = "loaded " + m_models.back();
    return true;
}

void ModelViewer::Update(float dt)
{
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.WantCaptureMouse) m_looking = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_looking = false;

    if (m_looking)
    {
        m_yaw += io.MouseDelta.x * 0.0025f;
        m_pitch -= io.MouseDelta.y * 0.0025f;
        m_pitch = std::max(-1.55f, std::min(1.55f, m_pitch));
    }

    if (!io.WantCaptureMouse && io.MouseWheel != 0.0f)
        m_speed = std::max(1.0f, m_speed * std::pow(1.15f, io.MouseWheel));

    if (!io.WantTextInput)
    {
        const XMVECTOR fwd = XMVectorSet(std::sin(m_yaw) * std::cos(m_pitch), std::sin(m_pitch), std::cos(m_yaw) * std::cos(m_pitch), 0);
        const XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, fwd));

        XMVECTOR move = XMVectorZero();
        if (ImGui::IsKeyDown(ImGuiKey_W)) move += fwd;
        if (ImGui::IsKeyDown(ImGuiKey_S)) move -= fwd;
        if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
        if (ImGui::IsKeyDown(ImGuiKey_E)) move += up;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move -= up;

        if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0f)
        {
            const float boost = ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 4.0f : 1.0f;
            XMVECTOR pos = XMLoadFloat3(&m_pos) + XMVector3Normalize(move) * (m_speed * boost * dt);
            XMStoreFloat3(&m_pos, pos);
        }
    }
}

void ModelViewer::DrawUI()
{
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(fs, fs), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(fs * 28.0f, fs * 42.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("Model viewer");

    ImGui::InputInt("Scenegraph index", &m_sgIndex);
    if (ImGui::Button("Load (replace)")) LoadModel((u32)std::max(0, m_sgIndex), false);
    ImGui::SameLine();
    if (ImGui::Button("Add to scene")) LoadModel((u32)std::max(0, m_sgIndex), true);
    ImGui::TextDisabled("Conker = 4, tail = 47");
    ImGui::TextWrapped("%s", m_status.c_str());

    ImGui::Checkbox("Wireframe", &m_wire);
    ImGui::SameLine();
    ImGui::Checkbox("Cull back faces", &m_cullBack);
    ImGui::SameLine();
    ImGui::Checkbox("Skeleton", &m_showBones);
    ImGui::SameLine();
    ImGui::Checkbox("Lighting", &m_lighting);
    ImGui::SliderFloat("Camera speed", &m_speed, 1.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("Reset camera")) ResetCamera();
    ImGui::TextDisabled("Right mouse = look, WASD move, Q/E down/up, Shift fast, wheel = speed");

    if (!m_subs.empty())
    {
        ImGui::Separator();
        for (const std::string& m : m_models) ImGui::BulletText("%s", m.c_str());
        ImGui::Text("Bounds: x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f",
            m_bmin[0], m_bmax[0], m_bmin[1], m_bmax[1], m_bmin[2], m_bmax[2]);

        std::vector<std::string> names;
        names.push_back("(none)");
        for (const TexSlot& t : m_tex) names.push_back(t.name + (t.srv ? "" : "  [not loaded]"));
        std::vector<const char*> items;
        for (const std::string& n : names) items.push_back(n.c_str());

        ImGui::TextDisabled("Textures are guessed from the push buffer texture format; change per draw if wrong.");
        if (ImGui::BeginChild("draws", ImVec2(0, fs * 16.0f), true))
        {
            for (size_t i = 0; i < m_subs.size(); ++i)
            {
                GpuSubmesh& s = m_subs[i];
                ImGui::PushID((int)i);
                ImGui::Checkbox("##vis", &s.visible);
                ImGui::SameLine();
                ImGui::Text("%s  %u tris", s.label.c_str(), s.indexCount / 3);
                int combo = s.texSlot + 1;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##tex", &combo, items.data(), (int)items.size())) s.texSlot = combo - 1;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        if (!m_bones.empty() && ImGui::TreeNode("Skeleton (bind pose positions)"))
        {
            for (const ModelBone& b : m_bones)
                ImGui::Text("%2d  parent %2d  at %7.2f %7.2f %7.2f", b.index, b.parent, b.world[0], b.world[1], b.world[2]);
            ImGui::TreePop();
        }

        if (!m_notes.empty() && ImGui::TreeNode("Decoder notes"))
        {
            for (const std::string& n : m_notes) ImGui::TextWrapped("%s", n.c_str());
            ImGui::TreePop();
        }
    }

    ImGui::End();
}

void ModelViewer::Render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv)
{
    if (!m_ready || m_subs.empty() || !rtv) return;

    // size of the back buffer, taken from the render target itself
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(res.As(&bb))) return;
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    if (td.Width == 0 || td.Height == 0) return;

    EnsureDepth(td.Width, td.Height);
    if (!m_dsv) return;

    ID3D11RenderTargetView* rt = rtv;
    ctx->OMSetRenderTargets(1, &rt, m_dsv.Get());
    ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_VIEWPORT vp = { 0, 0, (float)td.Width, (float)td.Height, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);

    // camera (left-handed, y up)
    const XMVECTOR fwd = XMVectorSet(std::sin(m_yaw) * std::cos(m_pitch), std::sin(m_pitch), std::cos(m_yaw) * std::cos(m_pitch), 0);
    const XMMATRIX view = XMMatrixLookToLH(XMLoadFloat3(&m_pos), fwd, XMVectorSet(0, 1, 0, 0));
    const float radius = std::max(1.0f, m_speed);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), (float)td.Width / (float)td.Height,
        std::max(0.05f, radius * 0.002f), radius * 200.0f);

    VSConstants vc;
    XMStoreFloat4x4(&vc.wvp, XMMatrixTranspose(view * proj));
    ctx->UpdateSubresource(m_cbVS.Get(), 0, nullptr, &vc, 0, 0);

    ctx->IASetInputLayout(m_layout.Get());
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, m_cbVS.GetAddressOf());
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(1, 1, m_cbPS.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    ctx->RSSetState(m_raster[m_wire ? 1 : 0][m_cullBack ? 1 : 0].Get());
    ctx->OMSetDepthStencilState(m_depthState.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // headlight: the light comes from the camera, so lit faces are the ones facing you
    XMFLOAT3 lightDir;
    XMStoreFloat3(&lightDir, XMVectorNegate(XMVector3Normalize(fwd)));

    // ---- triangles
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(ModelVertex), offset = 0;
    for (const GpuSubmesh& s : m_subs)
    {
        if (!s.visible || s.indexCount == 0) continue;

        ID3D11ShaderResourceView* srv = (s.texSlot >= 0 && s.texSlot < (int)m_tex.size()) ? m_tex[s.texSlot].srv.Get() : nullptr;
        PSConstants pc = { { 0.7f, 0.7f, 0.7f, 1.0f }, { lightDir.x, lightDir.y, lightDir.z, 0.0f }, srv ? 1.0f : 0.0f, m_lighting ? 1.0f : 0.0f, { 0, 0 } };
        ctx->UpdateSubresource(m_cbPS.Get(), 0, nullptr, &pc, 0, 0);
        ctx->PSSetShaderResources(0, 1, &srv);

        ID3D11Buffer* vb = m_buffers[s.bufferIndex].vb.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetIndexBuffer(s.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(s.indexCount, 0, 0);
    }

    // ---- skeleton lines, drawn on top of everything
    if (m_showBones && m_boneVB && m_boneVertexCount > 0)
    {
        PSConstants pc = { { 1.0f, 0.9f, 0.1f, 1.0f }, { 0, 0, 0, 0 }, 0.0f, 0.0f, { 0, 0 } };
        ctx->UpdateSubresource(m_cbPS.Get(), 0, nullptr, &pc, 0, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetDepthStencilState(m_depthNoTest.Get(), 0);
        ctx->RSSetState(m_raster[0][0].Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ID3D11Buffer* vb = m_boneVB.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->Draw(m_boneVertexCount, 0);
    }

    // leave the state the way ImGui expects it: back buffer only, no depth
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(1, &rt, nullptr);
}
#pragma once
#include "Package.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

struct GpuTexture
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    UINT        width = 0, height = 0;
    UINT        mipLevels = 0;
    UINT        arraySize = 1;      // layers from texture header +64
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool        isCube = false;     // heuristic: 6 layers and square (see TextureLoader.cpp)
};

// Builds a D3D11 texture (Texture2D / Texture2DArray / TextureCube) from a "texture" asset.
// On failure returns false and (if err != nullptr) fills in a reason.
bool CreateTexture(ID3D11Device* device, const Package& pkg, const Asset& asset,
    GpuTexture& out, std::string* err = nullptr);
#pragma once
#include "Package.hpp"
#include "TextureLoader.hpp"
#include <string>
#include <vector>

struct TextureTestEntry
{
    u32         index = 0;
    std::string path;
    bool        ok = false;
    std::string error;
    UINT        width = 0, height = 0, mips = 0, layers = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool        cube = false;
    GpuTexture  gpu;            // kept alive so you can preview it later
};

struct TextureTestReport
{
    bool        packageLoaded = false;
    std::string packageError;
    int         okCount = 0;
    int         failCount = 0;
    std::vector<TextureTestEntry> entries;
};

// Loads the package and tries to create every asset of type "texture".
// Failures are also written to the debugger output (OutputDebugString).
void RunTextureTest(ID3D11Device* device, Package& pkg, const std::string& packagePath, TextureTestReport& report);

// ImGui window: summary line + table (tick "failures only" to hide the good ones).
void DrawTextureTestWindow(const TextureTestReport& report);
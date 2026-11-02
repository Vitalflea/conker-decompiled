#pragma once
// Decodes the static geometry of a "scenegraph" asset (vertex arrays + NV2A push buffer strips).
// No D3D here: this produces plain CPU-side data that ModelViewer uploads.
// Verified on Conker (index 4) and the tail (index 47); see scenegraph_format_notes.md.
#include "Package.hpp"
#include <string>
#include <vector>

struct ModelVertex
{
    float x, y, z;
    float u, v;
};

// One vertex array set, shared by every strip that references the same position array.
struct ModelBuffer
{
    u32 positionTag = 0;
    float posScale = 0;
    float uvScale = 0;
    bool  hasUv = false;
    std::vector<ModelVertex> vertices;
};

struct ModelSubmesh
{
    u32 bufferIndex = 0;
    u32 pushBufferOffset = 0;       // gpu-relative offset of the 0x17FC command (debug)
    u32 op = 0;                     // NV2A primitive op (6 = strip, 5 = triangles, 7 = fan)
    std::vector<u32> indices;       // triangle list
    std::string label;
};

struct ModelData
{
    u32 assetIndex = 0;
    std::vector<ModelBuffer> buffers;
    std::vector<ModelSubmesh> submeshes;
    std::vector<std::string> textureNames;   // "texturefileN" values, e.g. "F_Conker_Main.1.tga"
    std::vector<u32> textureAssets;          // asset index of each texture in the package, 0 if not found
    float boundsMin[3] = { 0, 0, 0 };
    float boundsMax[3] = { 0, 0, 0 };
    std::vector<std::string> notes;          // things the decoder skipped or could not handle
};

bool LoadModelData(const Package& pkg, const Asset& scenegraph, ModelData& out, std::string* err = nullptr);

// "F_Conker_Main.1.tga" -> index of the texture asset whose path contains "\F_Conker_Main.1\", or 0.
u32 FindTextureAssetByName(const Package& pkg, const std::string& tgaName);

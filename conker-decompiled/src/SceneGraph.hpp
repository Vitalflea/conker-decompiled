#pragma once
#define NOMINMAX
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
    float nx, ny, nz;       // unit normal; (0,0,0) when the buffer has no supported normals
};

// One vertex array set, shared by every strip that references the same position array.
struct ModelBuffer
{
    u32 positionTag = 0;
    float posScale = 0;
    float uvScale = 0;
    bool  hasUv = false;
    bool  hasNormals = false;
    std::vector<ModelVertex> vertices;
};

struct ModelSubmesh
{
    u32 bufferIndex = 0;
    u32 pushBufferOffset = 0;       // gpu-relative offset of the 0x17FC command (debug)
    u32 op = 0;                     // NV2A primitive op (6 = strip, 5 = triangles, 7 = fan)
    std::vector<u32> indices;       // triangle list
    bool hasStage1 = false;         // texture stage 1 (the diffuse stage) was set up before this draw
    u32  stage1Tag = 0;             // runtime patch tag the game replaces with the real texture address
    u32  stage1Format = 0;          // NV2A texture format word: colour format, mip count, log2 width/height
    std::string label;
};

// One joint from the ndSkeleton node. Axes match the mesh: x = lateral, y = up, z = forward, same units as the mesh.
struct ModelBone
{
    int   index = 0;
    int   fileParent = -1;      // the "parent" short stored in the record (does not always match the geometry)
    int   parent = -1;          // parent found from the geometry: world - parentWorld == local (-1 if none matches)
    float local[3] = { 0, 0, 0 };
    float world[3] = { 0, 0, 0 };   // bind pose position
};

struct ModelData
{
    u32 assetIndex = 0;
    std::vector<ModelBuffer> buffers;
    std::vector<ModelSubmesh> submeshes;
    std::vector<std::string> textureNames;   // "texturefileN" values, e.g. "F_Conker_Main.1.tga"
    std::vector<std::string> textureStatus;
    std::vector<u32> textureAssets;          // asset index of each texture in the package, 0 if not found
    float boundsMin[3] = { 0, 0, 0 };
    float boundsMax[3] = { 0, 0, 0 };
    std::vector<ModelBone> bones;            // bind pose skeleton (positions only, rotations are not decoded)
    std::vector<std::string> notes;          // things the decoder skipped or could not handle
};

bool LoadModelData(const Package& pkg, const Asset& scenegraph, ModelData& out, std::string* err = nullptr);

// "F_Conker_Main.1.tga" -> index of the texture asset whose path contains "\F_Conker_Main.1\", or 0.
u32 FindTextureAssetByName(const Package& pkg, const std::string& tgaName);
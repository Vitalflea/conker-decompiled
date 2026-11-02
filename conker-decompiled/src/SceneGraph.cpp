#include "SceneGraph.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <map>

namespace
{
    u32 rd32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
    float rdf(const u8* p) { float v; std::memcpy(&v, p, 4); return v; }
    int16_t rd16s(const u8* p) { int16_t v; std::memcpy(&v, p, 2); return v; }

    struct TagEntry
    {
        std::string label;
        u32 gpuRel = 0;     // offset inside the asset's .gpu part
        u32 size = 0;       // bytes
        u32 elem = 0;       // bytes per element
    };

    bool isPow2Fraction(float x)
    {
        if (!(x > 0.0f)) return false;
        int e;
        return std::frexp(x, &e) == 0.5f;
    }

    std::string readCString(const Package& pkg, u32 absoluteOffset, size_t maxLen)
    {
        u32 left = 0;
        const u8* p = pkg.resolve(absoluteOffset, &left);
        if (!p) return std::string();
        size_t n = 0;
        while (n < left && n < maxLen && p[n]) ++n;
        return std::string((const char*)p, n);
    }

    std::string lower(std::string s)
    {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // NV2A "CMP" vertex format (type 6): 11:11:10 packed signed-normalised, x in the low 11 bits, then y, then z.
    void decodeCmp(u32 w, float& x, float& y, float& z)
    {
        auto sext = [](u32 v, int bits) { int s = (int)v; if (v & (1u << (bits - 1))) s -= (1 << bits); return s; };
        x = sext(w & 0x7FF, 11) / 1023.0f;
        y = sext((w >> 11) & 0x7FF, 11) / 1023.0f;
        z = sext(w >> 22, 10) / 511.0f;
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1e-6f) { x /= len; y /= len; z /= len; }
    }

    // Converts one NV2A draw (primitive op + index list) into triangles. Returns false for unsupported ops.
    bool toTriangles(u32 op, const std::vector<u32>& idx, u32 vertexCount, std::vector<u32>& out)
    {
        auto valid = [&](u32 a, u32 b, u32 c) {
            return a != b && b != c && a != c && a < vertexCount && b < vertexCount && c < vertexCount;
            };
        switch (op)
        {
        case 5: // triangles
            for (size_t i = 0; i + 2 < idx.size(); i += 3)
                if (valid(idx[i], idx[i + 1], idx[i + 2]))
                {
                    out.push_back(idx[i]); out.push_back(idx[i + 1]); out.push_back(idx[i + 2]);
                }
            return true;
        case 6: // triangle strip, winding alternates
            for (size_t i = 0; i + 2 < idx.size(); ++i)
            {
                u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (!valid(a, b, c)) continue;
                if (i & 1) std::swap(a, b);
                out.push_back(a); out.push_back(b); out.push_back(c);
            }
            return true;
        case 7: // triangle fan
            for (size_t i = 1; i + 1 < idx.size(); ++i)
                if (valid(idx[0], idx[i], idx[i + 1]))
                {
                    out.push_back(idx[0]); out.push_back(idx[i]); out.push_back(idx[i + 1]);
                }
            return true;
        }
        return false;
    }
}

u32 FindTextureAssetByName(const Package& pkg, const std::string& tgaName)
{
    std::string stem = tgaName;
    const std::string ext = ".tga";
    if (stem.size() > ext.size() && lower(stem.substr(stem.size() - ext.size())) == ext)
        stem.erase(stem.size() - ext.size());
    if (stem.empty()) return 0;

    const std::string needle = "\\" + lower(stem) + "\\";
    for (const Asset& a : pkg.assets())
    {
        if (a.index == 0 || a.type != "texture" || a.path.empty()) continue;
        std::string p = lower(a.path);
        std::replace(p.begin(), p.end(), '/', '\\');
        if (p.find(needle) != std::string::npos) return a.index;
    }
    return 0;
}

bool LoadModelData(const Package& pkg, const Asset& a, ModelData& out, std::string* err)
{
    out = ModelData();
    out.assetIndex = a.index;

    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    if (a.type != "scenegraph") return fail("asset " + std::to_string(a.index) + " is not a scenegraph");
    if (!a.data || !a.gpu || a.dataSize < 64 || a.gpuSize < 4) return fail("scenegraph has no .data/.gpu");

    const u8* d = a.data;
    const u32 dsz = a.dataSize;
    const u8* g = a.gpu;
    const u32 gsz = a.gpuSize;

    // ---- 1. vertex attribute tables: label string pointer -> entry {elem, ptr, 1, tag, 0, gpuPtr, size}
    std::unordered_map<u32, std::string> labels;
    for (u32 i = 0; i + 3 < dsz; ++i)
    {
        if (d[i] != 'v' || d[i + 1] != ':') continue;
        u32 j = i + 2;
        while (j < dsz && (std::isalnum(d[j]) || d[j] == '_')) ++j;
        if (j < dsz && d[j] == 0 && j > i + 2)
            labels[a.dataOffset + i] = std::string((const char*)d + i, j - i);
    }

    std::unordered_map<u32, TagEntry> tags;
    for (u32 o = 4; o + 24 <= dsz; o += 4)
    {
        auto it = labels.find(rd32(d + o));
        if (it == labels.end()) continue;

        const u8* e = d + o - 4;
        const u32 elem = rd32(e), one = rd32(e + 8), tag = rd32(e + 12), zero = rd32(e + 16);
        const u32 gp = rd32(e + 20), size = rd32(e + 24);
        if (one != 1 || zero != 0) continue;
        if (gp < a.gpuOffset || gp >= a.gpuOffset + gsz) continue;

        TagEntry t;
        t.label = it->second;
        t.gpuRel = gp - a.gpuOffset;
        t.size = size;
        t.elem = elem;
        tags[tag] = t;
    }
    if (tags.empty()) return fail("no vertex attribute tables found");

    // ---- 2. texture names ("texturefileN" rows; a row's value is stored in the *next* row)
    for (u32 o = 0; o + 12 <= dsz; o += 4)
    {
        const u32 v = rd32(d + o);
        if (v < 0x100) continue;
        const std::string s = readCString(pkg, v, 16);
        if (s.size() < 12 || s.compare(0, 11, "texturefile") != 0 || !std::isdigit((unsigned char)s[11])) continue;
        const int n = std::atoi(s.c_str() + 11);
        if (n < 0 || n >= 64) continue;
        const std::string val = readCString(pkg, rd32(d + o + 8), 128);
        if ((int)out.textureNames.size() <= n) out.textureNames.resize(n + 1);
        out.textureNames[n] = val;
    }
    out.textureAssets.assign(out.textureNames.size(), 0);
    for (size_t i = 0; i < out.textureNames.size(); ++i)
        out.textureAssets[i] = FindTextureAssetByName(pkg, out.textureNames[i]);

    // ---- 3. push buffer array: starts with NV097_NO_OPERATION (noinc, 576 bytes) = 00 01 40 42
    static const u8 marker[4] = { 0x00, 0x01, 0x40, 0x42 };
    u32 start = 0xFFFFFFFF;
    for (u32 i = 0; i + 4 <= gsz; i += 4)
        if (std::memcmp(g + i, marker, 4) == 0) { start = i; break; }
    if (start == 0xFFFFFFFF) return fail("push buffer marker 0x42400100 not found in .gpu");

    // ---- 4. walk the push buffer collecting draws
    struct Draw
    {
        bool active = false;
        u32 op = 0, at = 0;
        std::vector<u32> idx;
        u32 posTag = 0, uvTag = 0, nrmTag = 0;
        bool hasPos = false, hasUv = false, hasNrm = false;
        float posScale = 0, uvScale = 0;
        bool hasStage1 = false;
        u32 stage1Tag = 0, stage1Format = 0;
    } cur;

    u32 slots[16] = {};
    u32 fmts[16] = {};
    bool haveStage1 = false;
    u32 stage1Tag = 0, stage1Format = 0;
    std::map<u32, float> consts;         // constant register -> x of scale-looking constants
    u32 curReg = 0;
    bool haveReg = false;
    float lastPos = 1.0f / 32.0f, lastUv = 1.0f / 16384.0f;

    std::unordered_map<u32, u32> bufferOf;   // position tag -> index into out.buffers
    std::vector<int> stripCount;

    auto finishDraw = [&]()
        {
            if (!cur.active) return;
            cur.active = false;
            if (!cur.hasPos) { out.notes.push_back("draw at gpu+" + std::to_string(cur.at) + ": no v:position array bound"); return; }

            const TagEntry& pe = tags[cur.posTag];
            if (pe.elem != 6) { out.notes.push_back("draw at gpu+" + std::to_string(cur.at) + ": position element size " + std::to_string(pe.elem) + " (only 3 x s16 supported)"); return; }

            u32 bi;
            auto it = bufferOf.find(cur.posTag);
            if (it == bufferOf.end())
            {
                ModelBuffer mb;
                mb.positionTag = cur.posTag;
                mb.posScale = cur.posScale;
                mb.uvScale = cur.uvScale;
                const u32 n = pe.size / 6;
                if ((size_t)pe.gpuRel + (size_t)n * 6 > gsz) { out.notes.push_back("position array runs past .gpu"); return; }

                u32 un = 0;
                const TagEntry* ue = nullptr;
                if (cur.hasUv)
                {
                    ue = &tags[cur.uvTag];
                    if (ue->elem == 4 && (size_t)ue->gpuRel + ue->size <= gsz) { un = ue->size / 4; mb.hasUv = true; }
                }

                u32 nn = 0;
                const TagEntry* ne = nullptr;
                if (cur.hasNrm)
                {
                    ne = &tags[cur.nrmTag];
                    if (ne->elem == 4 && (size_t)ne->gpuRel + ne->size <= gsz) { nn = ne->size / 4; mb.hasNormals = true; }
                }

                mb.vertices.resize(n);
                for (u32 v = 0; v < n; ++v)
                {
                    const u8* p = g + pe.gpuRel + 6 * v;
                    ModelVertex& mv = mb.vertices[v];
                    mv.x = rd16s(p) * mb.posScale;
                    mv.y = rd16s(p + 2) * mb.posScale;
                    mv.z = rd16s(p + 4) * mb.posScale;
                    mv.u = mv.v = 0;
                    mv.nx = mv.ny = mv.nz = 0;
                    if (mb.hasNormals && v < nn)
                        decodeCmp(rd32(g + ne->gpuRel + 4 * v), mv.nx, mv.ny, mv.nz);
                    if (mb.hasUv && v < un)
                    {
                        const u8* q = g + ue->gpuRel + 4 * v;
                        mv.u = rd16s(q) * mb.uvScale;
                        mv.v = rd16s(q + 2) * mb.uvScale;
                    }
                }
                bi = (u32)out.buffers.size();
                bufferOf[cur.posTag] = bi;
                out.buffers.push_back(std::move(mb));
                stripCount.push_back(0);
            }
            else bi = it->second;

            ModelSubmesh sm;
            sm.bufferIndex = bi;
            sm.pushBufferOffset = cur.at;
            sm.op = cur.op;
            sm.hasStage1 = cur.hasStage1;
            sm.stage1Tag = cur.stage1Tag;
            sm.stage1Format = cur.stage1Format;
            if (!toTriangles(cur.op, cur.idx, (u32)out.buffers[bi].vertices.size(), sm.indices))
            {
                out.notes.push_back("draw at gpu+" + std::to_string(cur.at) + ": unsupported primitive op " + std::to_string(cur.op));
                return;
            }
            sm.label = "buffer " + std::to_string(bi) + " draw " + std::to_string(stripCount[bi]++) +
                " (gpu+" + std::to_string(cur.at) + ")";
            if (!sm.indices.empty()) out.submeshes.push_back(std::move(sm));
        };

    size_t p = start;
    while (p + 4 <= gsz)
    {
        const u32 h = rd32(g + p);
        if ((h & 3) != 0 || (h & 0xE0000000u) == 0x20000000u)
        {
            out.notes.push_back("push buffer jump/call at gpu+" + std::to_string(p) + ", stopped decoding there");
            break;
        }
        const u32 m = h & 0x1FFC;
        const u32 c = (h >> 18) & 0x7FF;
        if (p + 4 + (size_t)c * 4 > gsz)
        {
            out.notes.push_back("push buffer command runs past .gpu at gpu+" + std::to_string(p));
            break;
        }
        const u8* w = g + p + 4;

        switch (m)
        {
        case 0x1720:    // NV097_SET_VERTEX_DATA_ARRAY_OFFSET x16: tags of the bound arrays
            for (u32 i = 0; i < 16; ++i) slots[i] = i < c ? rd32(w + 4 * i) : 0;
            break;

        case 0x1B40:    // NV097_SET_TEXTURE_OFFSET (+ FORMAT in the second word) for stage 1
            if (c >= 2) { stage1Tag = rd32(w); stage1Format = rd32(w + 4); haveStage1 = true; }
            break;

        case 0x1760:    // NV097_SET_VERTEX_DATA_ARRAY_FORMAT x16: type (low 4 bits), size, stride
            for (u32 i = 0; i < 16; ++i) fmts[i] = i < c ? rd32(w + 4 * i) : 0;
            break;

        case 0x1EA4:    // transform constant load index
            if (c >= 1) { curReg = rd32(w); haveReg = true; }
            break;

        case 0x0B80:    // transform constants, 4 floats each; y/z words carry junk, w = 1.0
            if (haveReg)
            {
                for (u32 k = 0; k + 4 <= c; k += 4)
                {
                    const float x = rdf(w + 4 * k), y = rdf(w + 4 * k + 4), z = rdf(w + 4 * k + 8), ww = rdf(w + 4 * k + 12);
                    const u32 reg = curReg++;
                    if (std::fabs(y) < 1e-20f && std::fabs(z) < 1e-20f && ww == 1.0f &&
                        isPow2Fraction(x) && x >= 9.5367431640625e-07f /* 2^-20 */ && x <= 0.125f)
                        consts[reg] = x;
                    else
                        consts.erase(reg);
                }
            }
            break;

        case 0x17FC:    // NV097_SET_BEGIN_END
            if (c >= 1 && rd32(w) != 0)
            {
                finishDraw();
                cur = Draw();
                cur.active = true;
                cur.op = rd32(w);
                cur.at = (u32)p;
                cur.hasStage1 = haveStage1;
                cur.stage1Tag = stage1Tag;
                cur.stage1Format = stage1Format;
                for (u32 i = 0; i < 16; ++i)
                {
                    auto it = tags.find(slots[i]);
                    if (it == tags.end()) continue;
                    if (!cur.hasPos && it->second.label == "v:position") { cur.posTag = slots[i]; cur.hasPos = true; }
                    if (!cur.hasUv && it->second.label == "v:tex0") { cur.uvTag = slots[i]; cur.hasUv = true; }
                    // only the packed 11:11:10 format (type 6) is understood
                    if (!cur.hasNrm && it->second.label == "v:normal" && (fmts[i] & 0xF) == 6) { cur.nrmTag = slots[i]; cur.hasNrm = true; }
                }

                // position scale = larger scale-looking constant, uv scale = smaller one
                float hi = 0, lo = 1e9f;
                for (const auto& kv : consts) { hi = std::max(hi, kv.second); lo = std::min(lo, kv.second); }
                if (consts.size() >= 2 && hi > lo * 4.0f) { cur.posScale = hi; cur.uvScale = lo; }
                else if (consts.size() == 1)
                {
                    if (hi >= 1.0f / 512.0f) { cur.posScale = hi; cur.uvScale = lastUv; }
                    else { cur.posScale = lastPos; cur.uvScale = hi; }
                }
                else { cur.posScale = lastPos; cur.uvScale = lastUv; }
                lastPos = cur.posScale;
                lastUv = cur.uvScale;
                consts.clear();
            }
            else
            {
                finishDraw();
            }
            break;

        case 0x1800:    // ARRAY_ELEMENT16: two 16-bit indices per word
            if (cur.active)
                for (u32 k = 0; k < c; ++k)
                {
                    const u32 x = rd32(w + 4 * k);
                    cur.idx.push_back(x & 0xFFFF);
                    cur.idx.push_back(x >> 16);
                }
            break;

        case 0x1808:    // ARRAY_ELEMENT32
            if (cur.active)
                for (u32 k = 0; k < c; ++k) cur.idx.push_back(rd32(w + 4 * k));
            break;

        case 0x1810:    // DRAW_ARRAYS: first index in low 24 bits, count-1 in the top byte
            if (cur.active)
                for (u32 k = 0; k < c; ++k)
                {
                    const u32 x = rd32(w + 4 * k);
                    const u32 first = x & 0xFFFFFF, count = ((x >> 24) & 0xFF) + 1;
                    for (u32 j = 0; j < count; ++j) cur.idx.push_back(first + j);
                }
            break;
        }

        p += 4 + (size_t)c * 4;
    }
    finishDraw();

    if (out.submeshes.empty()) return fail("no triangles decoded");

    // ---- 5. skeleton: "ndSkeleton" node, 36-byte records starting 0x24 bytes after the node start:
    //         s16 parent, child, sibling, index, ?, ? ; float local[3]; float world[3]
    for (u32 i = 0; i + 0x24 + 36 <= dsz; i += 4)
    {
        if (std::memcmp(d + i, "ndSkeleton", 11) != 0) continue;
        u32 pos = i + 0x24;
        for (int n = 0; pos + 36 <= dsz && n < 256; ++n, pos += 36)
        {
            const int16_t sh[6] = { rd16s(d + pos), rd16s(d + pos + 2), rd16s(d + pos + 4), rd16s(d + pos + 6), rd16s(d + pos + 8), rd16s(d + pos + 10) };
            if (sh[3] != n || sh[0] < -1 || sh[0] >= 256 || sh[1] < -1 || sh[1] >= 256 || sh[2] < -1 || sh[2] >= 256) break;
            ModelBone b;
            b.index = n;
            b.fileParent = sh[0];
            for (int k = 0; k < 3; ++k) { b.local[k] = rdf(d + pos + 12 + 4 * k); b.world[k] = rdf(d + pos + 24 + 4 * k); }
            out.bones.push_back(b);
        }
        // parent from the geometry: world - parentWorld == local
        for (size_t k = 0; k < out.bones.size(); ++k)
            for (size_t p = 0; p < out.bones.size(); ++p)
            {
                if (p == k) continue;
                bool match = true;
                for (int a = 0; a < 3 && match; ++a)
                    match = std::fabs(out.bones[k].world[a] - out.bones[p].world[a] - out.bones[k].local[a]) < 0.02f;
                if (match) { out.bones[k].parent = (int)p; break; }
            }
        break;
    }

    // ---- 6. bounds over the buffers actually used
    bool first = true;
    for (const ModelSubmesh& sm : out.submeshes)
    {
        for (u32 i : sm.indices)
        {
            const ModelVertex& v = out.buffers[sm.bufferIndex].vertices[i];
            const float f[3] = { v.x, v.y, v.z };
            for (int k = 0; k < 3; ++k)
            {
                if (first || f[k] < out.boundsMin[k]) out.boundsMin[k] = f[k];
                if (first || f[k] > out.boundsMax[k]) out.boundsMax[k] = f[k];
            }
            first = false;
        }
    }
    return true;
}
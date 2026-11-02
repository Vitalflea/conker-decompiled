#include "TextureLoader.hpp"
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace
{
    // ------------------------------------------------------------------ texture header
    // Layout as found by checking all 183 textures (offsets are from the start of the asset's .data,
    // which begins with the 32-byte HEADER_TEMPLATE containing the "texture" name):
    //   +32 format   +44 width   +48 height   +52 .gpu pointer (absolute, already relocated)
    //   +56 mip count            +64 layer count
    // ASSUMPTION: width and height are two u32 (+44 and +48). If they are really two u16 at +44/+46,
    // change only parseHeader(); the sanity checks below will complain loudly if this is wrong.
    struct TexHeader
    {
        u32 format;
        u32 width, height;
        u32 gpuPtr;
        u32 mips;
        u32 layers;
    };

    u32 rd32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
    u16 rd16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }
    bool parseHeader(const Asset& a, TexHeader& h)
    {
        if (!a.data || a.dataSize < 68) return false;
        h.format = rd32(a.data + 32) & 0xFF;
        h.width = rd16(a.data + 44);
        h.height = rd16(a.data + 46);
        h.gpuPtr = rd32(a.data + 52);
        h.mips = rd32(a.data + 56);
        h.layers = rd32(a.data + 64);
        if (h.mips == 0)   h.mips = 1;
        if (h.layers == 0) h.layers = 1;
        return true;
    }

    // ------------------------------------------------------------------ formats
    enum : u32
    {
        XF_L8 = 0x00,
        XF_DXT1 = 0x0C,
        XF_DXT3 = 0x0E,
        XF_DXT5 = 0x0F,
        XF_A8L8 = 0x1A,
        XF_V8U8 = 0x28,
        XF_A8B8G8R8 = 0x3A,
    };

    struct FormatInfo
    {
        DXGI_FORMAT dxgi;
        u32  bytes;         // bytes per 4x4 block (compressed) or per pixel (uncompressed)
        bool compressed;
        bool swizzled;      // uncompressed Xbox formats are Morton-swizzled
    };

    bool getFormat(u32 xf, FormatInfo& f)
    {
        switch (xf)
        {
        case XF_DXT1:     f = { DXGI_FORMAT_BC1_UNORM,       8,  true,  false }; return true;
        case XF_DXT3:     f = { DXGI_FORMAT_BC2_UNORM,       16, true,  false }; return true;
        case XF_DXT5:     f = { DXGI_FORMAT_BC3_UNORM,       16, true,  false }; return true;
        case XF_L8:       f = { DXGI_FORMAT_R8_UNORM,        1,  false, true }; return true;  // L in .r
        case XF_A8L8:     f = { DXGI_FORMAT_R8G8_UNORM,      2,  false, true }; return true;  // L in .r, A in .g
        case XF_V8U8:     f = { DXGI_FORMAT_R8G8_SNORM,      2,  false, true }; return true;  // U in .r, V in .g
        case XF_A8B8G8R8: f = { DXGI_FORMAT_R8G8B8A8_UNORM,  4,  false, true }; return true;  // bytes are R,G,B,A
        }
        return false;
    }

    inline u32 mipDim(u32 v, u32 mip) { return std::max<u32>(1, v >> mip); }

    // bytes in one mip level and its row pitch
    void mipLayout(const FormatInfo& f, u32 w, u32 h, u32& pitch, u32& bytes)
    {
        if (f.compressed)
        {
            u32 bw = std::max<u32>(1, (w + 3) / 4);
            u32 bh = std::max<u32>(1, (h + 3) / 4);
            pitch = bw * f.bytes;
            bytes = pitch * bh;
        }
        else
        {
            pitch = w * f.bytes;
            bytes = pitch * h;
        }
    }

    // ------------------------------------------------------------------ Xbox unswizzle (UNVERIFIED)
    // Standard Xbox Morton layout: x bit, y bit, x bit, y bit ... until one dimension runs out,
    // the remaining bits of the longer dimension are linear above that.
    u32 depositBits(u32 v, u32 mask)
    {
        u32 r = 0, bit = 1;
        while (mask)
        {
            u32 low = mask & (~mask + 1);
            if (v & bit) r |= low;
            bit <<= 1;
            mask &= mask - 1;
        }
        return r;
    }

    void unswizzleMip(const u8* src, u8* dst, u32 w, u32 h, u32 bpp)
    {
        u32 mx = 0, my = 0, bit = 1, ww = w, hh = h;
        while (ww > 1 || hh > 1)
        {
            if (ww > 1) { mx |= bit; bit <<= 1; ww >>= 1; }
            if (hh > 1) { my |= bit; bit <<= 1; hh >>= 1; }
        }
        for (u32 y = 0; y < h; ++y)
        {
            u32 yo = depositBits(y, my);
            for (u32 x = 0; x < w; ++x)
            {
                u32 so = (depositBits(x, mx) | yo) * bpp;
                std::memcpy(dst + ((size_t)y * w + x) * bpp, src + so, bpp);
            }
        }
    }

    std::string hex(unsigned long v)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%lX", v);
        return buf;
    }

    bool setErr(std::string* err, const std::string& msg)
    {
        if (err) *err = msg;
        return false;
    }
}

bool CreateTexture(ID3D11Device* device, const Package& pkg, const Asset& asset,
    GpuTexture& out, std::string* err)
{
    const std::string name = "texture #" + std::to_string(asset.index) + ": ";

    if (asset.type != "texture")
        return setErr(err, name + "asset type is '" + asset.type + "', not texture");

    TexHeader h;
    if (!parseHeader(asset, h))
        return setErr(err, name + ".data too small for a texture header");

    FormatInfo fi;
    if (!getFormat(h.format, fi))
        return setErr(err, name + "unsupported format 0x" + hex(h.format));

    if (h.width == 0 || h.height == 0 || h.width > 8192 || h.height > 8192 || h.mips > 14 || h.layers > 2048)
        return setErr(err, name + "implausible header values (width/height/mips/layers) - check the header layout assumption in parseHeader()");

    if (fi.compressed && ((h.width % 4) || (h.height % 4)))
        return setErr(err, name + "BC textures need width/height multiple of 4");

    // ---- locate the pixel data
    u32 avail = 0;
    const u8* gpu = pkg.resolve(h.gpuPtr, &avail);
    if (!gpu)
    {
        gpu = asset.gpu;
        avail = asset.gpuSize;
    }
    if (!gpu)
        return setErr(err, name + "no .gpu data");

    // ---- one layer = full mip chain, mips packed back to back
    u32 chain = 0;
    for (u32 m = 0; m < h.mips; ++m)
    {
        u32 p, b;
        mipLayout(fi, mipDim(h.width, m), mipDim(h.height, m), p, b);
        chain += b;
    }

    // layer stride: total .gpu size / layers (this gives 896 for the cube map and 21888 for the 12-layer
    // DXT5, i.e. it includes the per-layer padding). Fall back to 128-byte alignment.
    u32 stride = chain;
    if (h.layers > 1)
    {
        const u32 total = asset.gpuSize ? asset.gpuSize : avail;
        if (total % h.layers == 0 && total / h.layers >= chain) stride = total / h.layers;
        else stride = (chain + 127) & ~127u;
    }

    const size_t needed = (size_t)stride * (h.layers - 1) + chain;
    if (needed > avail)
        return setErr(err, name + "mip chain x layers (" + std::to_string(needed) + " bytes) larger than .gpu data (" +
            std::to_string(avail) + ")");

    // ---- build subresource data: index = layer * mips + mip
    std::vector<D3D11_SUBRESOURCE_DATA> subs((size_t)h.mips * h.layers);
    std::vector<u8> scratch;                      // only used for swizzled formats
    if (fi.swizzled) scratch.resize((size_t)chain * h.layers);

    for (u32 layer = 0; layer < h.layers; ++layer)
    {
        size_t off = (size_t)stride * layer;
        size_t scratchOff = (size_t)chain * layer;
        for (u32 m = 0; m < h.mips; ++m)
        {
            u32 w = mipDim(h.width, m), hgt = mipDim(h.height, m), pitch, bytes;
            mipLayout(fi, w, hgt, pitch, bytes);

            D3D11_SUBRESOURCE_DATA& sd = subs[(size_t)layer * h.mips + m];
            if (fi.swizzled)
            {
                unswizzleMip(gpu + off, scratch.data() + scratchOff, w, hgt, fi.bytes);
                sd.pSysMem = scratch.data() + scratchOff;
                scratchOff += bytes;
            }
            else
            {
                sd.pSysMem = gpu + off;
            }
            sd.SysMemPitch = pitch;
            sd.SysMemSlicePitch = 0;
            off += bytes;
        }
    }

    // ---- create resource
    // Cube heuristic: exactly 6 layers and square. Faces are assumed +X,-X,+Y,-Y,+Z,-Z (same as D3D11).
    const bool cube = (h.layers == 6 && h.width == h.height);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = h.width;
    td.Height = h.height;
    td.MipLevels = h.mips;
    td.ArraySize = h.layers;
    td.Format = fi.dxgi;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&td, subs.data(), tex.GetAddressOf());
    if (FAILED(hr))
        return setErr(err, name + "CreateTexture2D failed, hr=0x" + hex((unsigned long)hr));

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = td.Format;
    if (cube)
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = h.mips;
        sv.TextureCube.MostDetailedMip = 0;
    }
    else if (h.layers > 1)
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = h.mips;
        sv.Texture2DArray.MostDetailedMip = 0;
        sv.Texture2DArray.FirstArraySlice = 0;
        sv.Texture2DArray.ArraySize = h.layers;
    }
    else
    {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = h.mips;
        sv.Texture2D.MostDetailedMip = 0;
    }

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &sv, srv.GetAddressOf());
    if (FAILED(hr))
        return setErr(err, name + "CreateShaderResourceView failed");

    out.texture = tex;
    out.srv = srv;
    out.width = h.width;
    out.height = h.height;
    out.mipLevels = h.mips;
    out.arraySize = h.layers;
    out.format = fi.dxgi;
    out.isCube = cube;
    return true;
}
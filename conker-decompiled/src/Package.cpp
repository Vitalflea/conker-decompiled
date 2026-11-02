#include "Package.hpp"
#include "Caff.hpp"           // decompress()
#include <fstream>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cctype>

#if !defined(_M_IX86) && !defined(__i386__)
#error "Package.cpp relies on the x86 inline-asm decompressor from caff.cpp: build as Win32 (x86)."
#endif

#ifdef RETAIL
static const size_t kHeaderSize = 384;
static const char* kVersion = "28.01.05.0031";
#else
static const size_t kHeaderSize = 344;
static const char* kVersion = "24.09.03.0026";
#endif

static std::string normalizePath(const std::string& p)
{
    std::string s = p;
    for (char& c : s)
    {
        if (c == '/') c = '\\';
        else c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

// read a NUL-terminated string without running past the end of the buffer
static std::string safeString(const u8* base, const u8* end, const u8* p, size_t maxLen = 512)
{
    if (p < base || p >= end) return std::string();
    size_t avail = (size_t)(end - p);
    size_t n = 0;
    size_t lim = avail < maxLen ? avail : maxLen;
    while (n < lim && p[n]) ++n;
    return std::string((const char*)p, n);
}

template <class T>
static T readAt(const u8* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

bool Package::load(const std::string& filename)
{
    m_assets.clear();
    m_byPath.clear();
    m_indexTable.clear();
    m_sections.clear();
    m_infos.clear();

    // ---------------------------------------------------------------- header
    std::ifstream f(filename, std::ios::binary);
    if (!f) return fail("cannot open " + filename);

    std::vector<char> hdr(kHeaderSize);
    f.read(hdr.data(), (std::streamsize)hdr.size());
    if ((size_t)f.gcount() != hdr.size()) return fail("file too small for header");
    if (std::strncmp(hdr.data(), "CAFF", 4) != 0) return fail("bad magic");
    if (std::strncmp(hdr.data() + 4, kVersion, 16) != 0) return fail("unexpected version string");

    std::memcpy(&m_header, hdr.data(), sizeof(CAFF_FILE_HEADER));
    if (m_header.numSections == 0) return fail("no sections");

    const size_t infoBytes = sizeof(CAFF_INFO_HEADER) * m_header.numSections;
    if (sizeof(CAFF_FILE_HEADER) + infoBytes > hdr.size()) return fail("section info does not fit in header");
    m_infos.resize(m_header.numSections);
    std::memcpy(m_infos.data(), hdr.data() + sizeof(CAFF_FILE_HEADER), infoBytes);

    // ---------------------------------------------------------------- sections
    m_sections.resize(m_header.numSections);
    f.seekg(m_header.dataOffset, std::ios::beg);
    for (size_t i = 0; i < m_sections.size(); ++i)
    {
        m_sections[i].resize(m_infos[i].sizeCompressed);
        f.read((char*)m_sections[i].data(), (std::streamsize)m_sections[i].size());
        if ((size_t)f.gcount() != m_sections[i].size()) return fail("truncated section data");
    }

    if (m_header.isCompressed)
    {
        for (size_t i = 0; i < m_sections.size(); ++i)
        {
            std::vector<u8> out(m_infos[i].sizeDecompressed);
            decompress((u32)(uintptr_t)m_sections[i].data(), m_infos[i].sizeCompressed, out.data());
            m_sections[i].swap(out);
        }
    }

    u8* d = m_sections[0].data();
    const u8* dEnd = d + m_sections[0].size();
    const u32 base = m_header.dataOffset;
    const u32 size0 = (u32)m_sections[0].size();

    // ---------------------------------------------------------------- tables inside .data
    const u32 toc = m_header.dataTOCOffset;
    if ((size_t)toc + 4 > m_sections[0].size()) return fail("TOC outside .data");

    const u32 nameBufSize = readAt<u32>(d + toc);
    const u8* nameOffsets = d + toc + 4;
    const u8* nameBuf = nameBufSize ? nameOffsets + 4 * (size_t)m_header.numFiles : nameOffsets;
    const u8* indexPtr = nameBuf + nameBufSize;
    const u8* unused1 = indexPtr + sizeof(FILE_INDEX_ENTRY) * (size_t)m_header.numFilesWithGpu;
    const u8* unused2 = unused1 + 12 * (size_t)m_header.unk_table1;
    const u8* relocPtr = unused2 + 4 * (size_t)m_header.unk_table2;
    const u8* relocOffs = relocPtr + 12 * (size_t)m_header.numRelocs;
    if (relocOffs > dEnd) return fail("tables run past end of .data");

    // ---------------------------------------------------------------- index table (absolute offsets)
    m_indexTable.resize(m_header.numFilesWithGpu);
    if (!m_indexTable.empty())
        std::memcpy(m_indexTable.data(), indexPtr, sizeof(FILE_INDEX_ENTRY) * m_indexTable.size());

    u32 maxIndex = m_header.numFiles;
    for (FILE_INDEX_ENTRY& e : m_indexTable)
    {
        if (e.filePart == 1)      e.fileOffset += base;
        else if (e.filePart == 2) e.fileOffset += base + size0;
        maxIndex = std::max(maxIndex, e.Index);
    }

    // ---------------------------------------------------------------- relocations (same rules as mapZPackageFile)
    const u8* ro = relocOffs;
    for (u32 i = 0; i < m_header.numRelocs; ++i)
    {
        RELOCATION_ENTRY r;
        std::memcpy(&r, relocPtr + 12 * (size_t)i, sizeof(r));

        const bool ok = r.sourceIndex >= 1 && r.sourceIndex <= m_indexTable.size() &&
            r.targetIndex >= 1 && r.targetIndex <= m_indexTable.size();
        const FILE_INDEX_ENTRY* src = ok ? &m_indexTable[r.sourceIndex - 1] : nullptr;
        const FILE_INDEX_ENTRY* tgt = ok ? &m_indexTable[r.targetIndex - 1] : nullptr;

        for (u32 j = 0; j < r.relocationCount; ++j, ro += 4)
        {
            if (ro + 4 > dEnd) return fail("relocation offsets run past end of .data");
            if (!ok || src->filePart != 1) continue;

            const u32 off = readAt<u32>(ro);
            if (off >= src->fileSize) continue;

            const size_t pos = (size_t)src->fileOffset + off - base;
            if (pos + 4 > m_sections[0].size()) continue;

            u32 v = readAt<u32>(d + pos);
            v += tgt->fileOffset;
            std::memcpy(d + pos, &v, 4);
        }
    }

    // ---------------------------------------------------------------- assets
    m_assets.assign((size_t)maxIndex + 1, Asset());
    for (const FILE_INDEX_ENTRY& e : m_indexTable)
    {
        Asset& a = m_assets[e.Index];
        a.index = e.Index;

        if (e.filePart == 1)
        {
            const size_t pos = (size_t)e.fileOffset - base;
            if (pos > m_sections[0].size()) continue;
            a.data = d + pos;
            a.dataSize = std::min<u32>(e.fileSize, (u32)(m_sections[0].size() - pos));
            a.dataOffset = e.fileOffset;

            std::string t = safeString(d, dEnd, a.data, 16);
            auto it = std::find(assetTypes.begin(), assetTypes.end(), t);
            a.type = (it != assetTypes.end()) ? t : "unknown";
        }
        else if (e.filePart == 2 && m_sections.size() > 1)
        {
            const size_t pos = (size_t)e.fileOffset - base - size0;
            if (pos > m_sections[1].size()) continue;
            a.gpu = m_sections[1].data() + pos;
            a.gpuSize = std::min<u32>(e.fileSize, (u32)(m_sections[1].size() - pos));
            a.gpuOffset = e.fileOffset;
        }
    }

    // c_filename (not present in this package, nameBufSize == 0)
    if (nameBufSize != 0)
    {
        for (u32 i = 0; i < m_header.numFiles; ++i)
        {
            u32 off = readAt<u32>(nameOffsets + 4 * (size_t)i);
            if (i + 1 < m_assets.size())
                m_assets[i + 1].cName = safeString(d, dEnd, nameBuf + off);
        }
    }

    // p_filename: the package table at the start of .data maps names -> .data offsets
    if (m_sections[0].size() >= sizeof(PACKAGE_HEADER) + 4)
    {
        const u32 count = readAt<u32>(d + sizeof(PACKAGE_HEADER));
        const u8* table = d + sizeof(PACKAGE_HEADER) + 4;
        for (u32 i = 0; i < count; ++i)
        {
            if (table + (size_t)(i + 1) * sizeof(PACKAGE_ENTRY) > dEnd) break;
            PACKAGE_ENTRY pe = readAt<PACKAGE_ENTRY>(table + (size_t)i * sizeof(PACKAGE_ENTRY));
            if (pe.fileNameOffset < base) continue;

            std::string name = safeString(d, dEnd, d + (pe.fileNameOffset - base));
            for (const FILE_INDEX_ENTRY& e : m_indexTable)
            {
                if (e.filePart == 1 && pe.dataOffset >= e.fileOffset && pe.dataOffset < e.fileOffset + e.fileSize)
                    m_assets[e.Index].path = name;
            }
        }
    }

    for (const Asset& a : m_assets)
        if (!a.path.empty())
            m_byPath[normalizePath(a.path)] = a.index;

    return true;
}

const Asset* Package::find(const std::string& path) const
{
    auto it = m_byPath.find(normalizePath(path));
    return it == m_byPath.end() ? nullptr : &m_assets[it->second];
}

const Asset* Package::findByIndex(u32 index) const
{
    if (index == 0 || index >= m_assets.size() || m_assets[index].index == 0) return nullptr;
    return &m_assets[index];
}

const u8* Package::resolve(u32 absoluteOffset, u32* bytesLeft) const
{
    if (m_sections.empty() || absoluteOffset < m_header.dataOffset) return nullptr;

    size_t rel = absoluteOffset - m_header.dataOffset;
    for (const std::vector<u8>& s : m_sections)
    {
        if (rel < s.size())
        {
            if (bytesLeft) *bytesLeft = (u32)(s.size() - rel);
            return s.data() + rel;
        }
        rel -= s.size();
    }
    return nullptr;
}
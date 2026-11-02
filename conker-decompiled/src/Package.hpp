#pragma once
// Runtime loader for retail CAFF packages (.rbm). Port of the FileHandler logic:
// read -> decompress -> index table -> relocations -> names -> asset table.
// NOTE: decompress() in caff.cpp is the game's x86 inline asm, so this must be built as Win32 (x86).
#include "Common.hpp"
#include <string>
#include <vector>
#include <unordered_map>

struct Asset
{
    u32 index = 0;              // 1-based, same as the "index" column of the listfile CSV
    std::string type;           // "texture", "model", "scenegraph", ... ("unknown" if not recognised)
    std::string path;           // p_filename (empty for assets that are only reached by pointer)
    std::string cName;          // c_filename (empty in this package)

    const u8* data = nullptr;   // .data part (nullptr if none)
    u32 dataSize = 0;
    u32 dataOffset = 0;         // absolute offset, same value as the CSV data_offset

    const u8* gpu = nullptr;    // .gpu part (nullptr if none)
    u32 gpuSize = 0;
    u32 gpuOffset = 0;          // absolute offset, same value as the CSV gpu_offset
};

class Package
{
public:
    bool load(const std::string& filename);

    // Case-insensitive, accepts '/' or '\'. Returns nullptr if not found.
    const Asset* find(const std::string& path) const;
    const Asset* findByIndex(u32 index) const;
    const std::vector<Asset>& assets() const { return m_assets; }

    // Turns a relocated "absolute offset" pointer found inside .data (e.g. the .gpu pointer at
    // texture header +52) into a real memory pointer. bytesLeft = bytes available after it.
    const u8* resolve(u32 absoluteOffset, u32* bytesLeft = nullptr) const;

    const std::string& lastError() const { return m_error; }

private:
    bool fail(const std::string& msg) { m_error = msg; return false; }

    CAFF_FILE_HEADER m_header = {};
    std::vector<CAFF_INFO_HEADER> m_infos;
    std::vector<std::vector<u8>> m_sections;        // [0] = .data, [1] = .gpu (decompressed)
    std::vector<FILE_INDEX_ENTRY> m_indexTable;     // offsets already made absolute
    std::vector<Asset> m_assets;                    // m_assets[i] has index i (slot 0 unused)
    std::unordered_map<std::string, u32> m_byPath;
    std::string m_error;
};
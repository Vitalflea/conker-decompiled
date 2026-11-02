#include "TextureTest.hpp"
#include "imgui.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
static const char* DxgiName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_BC1_UNORM:       return "BC1 (DXT1)";
    case DXGI_FORMAT_BC2_UNORM:       return "BC2 (DXT3)";
    case DXGI_FORMAT_BC3_UNORM:       return "BC3 (DXT5)";
    case DXGI_FORMAT_R8_UNORM:        return "R8 (L8)";
    case DXGI_FORMAT_R8G8_UNORM:      return "R8G8 (A8L8)";
    case DXGI_FORMAT_R8G8_SNORM:      return "R8G8 SNORM (V8U8)";
    case DXGI_FORMAT_R8G8B8A8_UNORM:  return "RGBA8 (A8B8G8R8)";
    default:                          return "-";
    }
}

static void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

void RunTextureTest(ID3D11Device* device, Package& pkg, const std::string& packagePath, TextureTestReport& report)
{
    report = TextureTestReport();

    if (!pkg.load(packagePath))
    {
        report.packageError = pkg.lastError();
        Log("[texture test] package load failed: %s\n", report.packageError.c_str());
        return;
    }
    report.packageLoaded = true;

    for (const Asset& a : pkg.assets())
    {
        if (a.index == 0 || a.type != "texture") continue;

        TextureTestEntry e;
        e.index = a.index;
        e.path = a.path;
        e.ok = CreateTexture(device, pkg, a, e.gpu, &e.error);
        if (e.ok)
        {
            e.width = e.gpu.width;
            e.height = e.gpu.height;
            e.mips = e.gpu.mipLevels;
            e.layers = e.gpu.arraySize;
            e.format = e.gpu.format;
            e.cube = e.gpu.isCube;
            ++report.okCount;
        }
        else
        {
            ++report.failCount;
            Log("[texture test] FAIL %s\n", e.error.c_str());
        }
        report.entries.push_back(std::move(e));
    }

    Log("[texture test] %d ok, %d failed, %d total\n", report.okCount, report.failCount,
        (int)report.entries.size());
}

void DrawTextureTestWindow(const TextureTestReport& r)
{
    static bool failuresOnly = false;

    ImGui::Begin("Texture test");

    if (!r.packageLoaded)
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Package load failed: %s", r.packageError.c_str());
        ImGui::End();
        return;
    }

    ImGui::Text("%d ok, %d failed, %d total", r.okCount, r.failCount, (int)r.entries.size());
    ImGui::Checkbox("Failures only", &failuresOnly);

    if (ImGui::BeginTable("textures", 7,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Index");
        ImGui::TableSetupColumn("Format");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Mips");
        ImGui::TableSetupColumn("Layers");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Name / error");
        ImGui::TableHeadersRow();

        for (const TextureTestEntry& e : r.entries)
        {
            if (failuresOnly && e.ok) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", e.index);
            if (e.ok)
            {
                ImGui::TableNextColumn(); ImGui::TextUnformatted(DxgiName(e.format));
                ImGui::TableNextColumn(); ImGui::Text("%ux%u", e.width, e.height);
                ImGui::TableNextColumn(); ImGui::Text("%u", e.mips);
                ImGui::TableNextColumn(); ImGui::Text("%u", e.layers);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.cube ? "cube" : (e.layers > 1 ? "array" : "2D"));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.path.empty() ? "(unnamed)" : e.path.c_str());
            }
            else
            {
                ImGui::TableNextColumn(); ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", e.error.c_str());
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
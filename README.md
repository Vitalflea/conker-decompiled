# Conker: Live & Reloaded Decompiled

An independent preservation and reverse-engineering project whose goal is now a **port of Conker: Live & Reloaded (original Xbox, Konami, 2005) to Windows x86**: its program logic recreated in human-readable C/C++ through independent analysis, built as a native Win32 application rendered with **DirectX 11** and driven by **Dear ImGui**.

The success bar is deliberately *not* byte-exactness. A unit counts as done when:

1. It **compiles** cleanly under MSVC as a Win32/x86 target.
2. It **works** — the running port exhibits the same observable behaviour as the retail game (state transitions, rendering, input handling, timing, audio playback), validated functionally rather than by matching emitted machine code.

Byte-level comparisons against the retail image remain useful as an analytical cross-check while dissecting functions, but they are no longer a completion criterion.

## Repository layout

```
conker-decompiled/
├── conker-decompiled.slnx            # Visual Studio solution
├── conker-decompiled/                # Reconstructed C++ project (MSVC)
│   ├── src/                        # Human-readable reconstruction sources
│   └── x64/Debug/                  # Build artifacts
└── iso/                            # Reference material (read-only ground truth)
    ├── Conker Live & Reloaded (Mar 14, 2005 prototype)/
    │   └── default.xbe             # Active analysis target (current IDA database)
    └── Conker Live & Reloaded (Apr 19 & Jul 01 2005 Prototype)/
        ├── Release.xbe             # Retail release executable
        ├── FinalRelease.xbe        # Alternate final build
        ├── Test.xbe / MemTest.xbe   # Development builds
        ├── media/                  # XNA assets (XSB/XWB fonts, audio)
        ├── dvddata/                # Runtime data files
        └── vince/                  # Debug log captures
```

The `iso/` tree is the reference the verifications run against. Multiple `.xbe` images are kept side by side so any build can be cross-checked against another; the active decompilation target is `default.xbe` from the Mar 14, 2005 prototype.

## Methodology

### 1. Reverse engineering ground truth
- IDA Pro analyzes the retail `.xbe` image via the **ida-pro-mcp** tools (`decompile`, `disasm`, `xrefs_to`, `callgraph`, `find_bytes`, ...).
- Xbox executables have no PE header or DLL import tables: structure comes from the XBE signature/header, the section table, and the IFSL import block (module name → size/checksum entries) inside the image. Entry points (`start`, kernel thunks) come from the header fields.
- IDA lays each section out at its *virtual* load address, not at its raw file offset, so any comparison against the shipped file maps through the section table.
- Cross-references and call graphs are mapped so every reconstructed function traces back to a known address range — this provenance record keeps the port auditable even though output bytes will differ.

### 2. Porting into Windows x86 C/C++
- Pseudocode from IDA is rewritten as idiomatic C/C++ targeting a Win32 32-bit process, preserving the original control flow and data layout where practical.
- Xbox kernel/XDK services are mapped onto their Windows equivalents:
  - Thread creation (`PsCreateSystemThreadEx`) → Win32 threads / `std::thread`.
  - D3D9 device and render state → **DirectX 11** (`ID3D11Device`, swap chains, shaders).
  - XACT/XMV/WMA media pipeline → WASAPI + WMA decoder for the shipped assets.
  - Input and system services → standard Win32 input APIs.
- Menus, overlays, and debug tooling in the port use **Dear ImGui** (rendered through a DX11 backend), both for game UI recreation and as instrumentation during development.
- Every ported unit still records the retail address(es) it was derived from.

### 3. Functional verification
- The port is run under MSVC (Win32/x86); correctness is judged by behaviour — startup sequence, frame output, state machines, timing — observed and compared against the retail build's documented behaviour.
- Optional byte-diffing remains available as a sanity cross-check while dissecting individual functions, but a mismatch never blocks completion.

## Tooling

| Component | Purpose |
|---|---|
| IDA Pro (+ ida-pro-mcp) | Disassembly, decompilation, cross-reference analysis of the retail `.xbe` |
| Local Python (raw-file forensics only) | Parses XBE/IFSL structures directly from disk; confirms the IDB holds the exact shipped bytes before function work begins |
| MSVC (VS toolset v145, Win32/x86) | Builds the running port |
| DirectX 11 | Rendering target (swap chain, pixel/vertex shaders replacing the original D3D9 pipeline) |
| Dear ImGui | In-game menus/overlays plus developer-facing debug overlay, rendered via the DX11 backend |
| Behavioural test harness | Runs the port and compares observable state/output against notes taken from the retail build |

Optional binary diffing against the retail image remains available during analysis but gates nothing.

## Status

- [x] Goal change adopted: byte-exactness retired in favour of a working Windows x86 port (DX11 + imgui)
- [x] Ground truth established: IDB matches the raw `default.xbe` region-for-region (≈7.5 MB verified); VA = file offset + 0x1000
- [x] Boot chain analyzed (`start` @ 0x426AD0 → Xbox thread wrapper → game startup routine near 0x16C7E8 → dispatcher `sub_233E0`); the current Windows boot-chain source is exploratory and not yet a validated game port
- [ ] Host scaffold made project-owned and stable (Win32 main-thread loop, DX11, ImGui debug/status panel)
- [ ] First bounded game-facing slice selected from verified IDA call graph and reproduced with an observable result
- [ ] Title/menu startup path and minimum required assets traced
- [ ] Asset pipeline wired up (fonts, audio, textures from the ISO)

## Notes

- The ISO content is kept read-only in this repository and serves as ground-truth input for the reverse engineering; it is never modified or redistributed.
- Findings are recorded as annotations on the IDA database plus the ported source tree, each unit annotated with its originating retail address(es).
- The port is an independent reimplementation of logic and assets' *use*; it does not modify the original binaries.
- Xbox startup and kernel behavior are analyzed to understand game initialization. Recreate only the services needed by a chosen game-facing slice; see `docs/startup-path.md` before extending the current `bootchain.cpp` prototype.

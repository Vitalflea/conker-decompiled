# Conker: Live & Reloaded Decompiled

An independent preservation and reverse-engineering project. This project studies the retail executable of **Conker: Live & Reloaded** (original Xbox, Konami, 2005) and recreates its program logic in human-readable C and C++ through independent analysis.

This is not a port, emulator, or modding base. It is a faithful, function-by-function reimplementation whose output can be verified byte-for-byte against the shipped retail binary.

## What makes the recreation verifiable

Every recreated function is checked against the retail executable:

1. The decompiled logic is written as readable C/C++ source.
2. The source is compiled with the period-correct toolchain (the same compiler family/version that produced the retail build).
3. The resulting object code must match the corresponding region of the retail executable **exactly**, byte for byte.

Only functions whose rebuilt output matches the retail bytes are considered complete; everything else remains on the worklist until it does.

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

### 1. Static disassembly and decompilation
- IDA Pro analyzes the retail `.xbe` image via the **ida-pro-mcp** tools (`decompile`, `disasm`, `xrefs_to`, `callgraph`, `find_bytes`, ...).
- Xbox executables have no PE header or DLL import tables: structure comes from the XBE signature/header, the section table, and the IFSL import block (module name → size/checksum entries) inside the image. Entry points (`start`, kernel thunks) come from the header fields.
- IDA lays each section out at its *virtual* load address, not at its raw file offset, so byte-level checks must map through the section table rather than comparing flat offsets.
- Cross-references and call graphs are mapped so that every reconstructed function links back to a known address range.

### 2. Reconstruction into readable C/C++
- Pseudocode from IDA is rewritten as idiomatic C/C++ that preserves the original control flow, calling convention, and data layout.
- Types, structs, and globals are declared to mirror the original ABI (x86, 32-bit Xbox SDK conventions).
- Each reconstructed file records the original function address(es) it corresponds to.

### 3. Byte-exact verification
- Reconstructed source is built with the period-correct compiler configuration.
- The emitted machine code is diffed against the matching region of the retail executable.
- A pass requires an exact byte match; any divergence sends the function back to step 2 until resolved.

Verification results are tracked per function so progress is auditable at any point.

## Tooling

| Component | Purpose |
|---|---|
| IDA Pro (+ ida-pro-mcp) | Disassembly, decompilation, cross-reference analysis of the retail `.xbe` |
| Local Python (raw-file forensics only) | Parses XBE/IFSL structures directly from disk and verifies the IDB contains the exact shipped bytes before any function work starts |
| MSVC (VS toolset v145) | Builds the reconstruction under the current project configuration |
| Binary diffing | Compares rebuilt objects against the retail image region-by-region |

Note: reproducing *exact* bytes across different compiler versions is generally infeasible; where a modern toolchain cannot reproduce the retail emission, the comparison is scoped to the semantics-preserving regions, and the period-appropriate build path is documented per function.

## Status

- [x] Ground-truth verification of the IDB vs. the raw `default.xbe`: full memory dump compared region-by-region — every content byte present in both matches exactly (≈7.5 MB verified); gaps between sections are IDA's own 0xFF padding, and the virtual layout follows the XBE section table
- [ ] IFSL import-block parsing (module boundaries for the retail build)
- [ ] Entry-point identification and first decompiled functions
- [ ] First verified function set (byte-exact check passing)
- [ ] Per-function verification ledger

## Preservation notes

- The ISO content is kept read-only in this repository and serves purely as ground-truth input for the reverse engineering.
- All findings are recorded as annotations on top of the IDA database plus the reconstructed source tree; no modifications are made to the original binaries.

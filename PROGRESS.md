# Progress ledger

Target binary under active study: **default.xbe** (Mar 14, 2005 prototype), via IDA Pro (ida-pro-mcp).

Current goal: **port to Windows x86** — native Win32 C/C++ built with MSVC, rendered with **DirectX 11**, UI/debug overlays via **Dear ImGui**. Completion = compiles and runs with behaviour matching the retail game; byte-exactness is retired as a requirement (kept only as optional cross-check).

## Done
| Item | Status | Notes |
|---|---|---|
| README & project charter | done | `README.md` at repo root |
| Image layout decode | done | VA = file offset + 0x1000; sections 0x1000..0x75E000; header fields at 0x920–0x934; name list 0x984; import entries stride-20 from 0xA60 — see `docs/image-layout.md` |
| Entry points | done | `start` @ 0x426AD0, `kernel_thunk` @ 0x558660 |
| Boot chain decompilation | done | start -> sub_425C71 (PsCreateSystemThreadEx) -> thread entry sub_425BD9 -> main routine sub_16C740 -> dispatcher sub_233E0; abort paths sub_42728E / sub_4237F2 — see `docs/startup-path.md` |
| Memory subsystem mapped | partial | custom debug malloc (sub_2A7B00/2A7E60/3DDF20), source-file/line tags on allocations (`D:\work\Bfd\Game\...`) |

## Next steps
1. Port scaffold: DX11 device/swap-chain init, Win32 window, main loop, input polling; Dear ImGui debug overlay wired in.
2. Kernel/XDK call mapping table: `PsCreateSystemThreadEx` → Win32 threads/`std::thread`; XACT/XMV/WMA media pipeline → WASAPI decoders; D3D9 render state → DX11 equivalents.
3. First executable slice: boot chain (`start` + wrappers `sub_4239F9` / `sub_4237F2` / `sub_4237C4` / `sub_42728E`) ported to C++ and run under MSVC (Win32/x86); validate startup sequence against the decompiled boot path.
4. Asset pipeline: extract and convert ISO assets (fonts, audio, textures) into formats the DX11 port consumes.
5. Continue IFSL/import enumeration and map `sub_233E0`'s object model (vtables at +8/+12) to define the class hierarchy to port next.

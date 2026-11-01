# Progress ledger

Target binary under active study: **default.xbe** (Mar 14, 2005 prototype), via IDA Pro (ida-pro-mcp).

Current goal: **port to Windows x86** — native Win32 C/C++ built with MSVC, rendered with **DirectX 11**, UI/debug overlays via **Dear ImGui**. Completion = compiles and runs with behaviour matching the retail game; byte-exactness is retired as a requirement (kept only as optional cross-check).

## Done
| Item | Status | Notes |
|---|---|---|
| README & project charter | done | `README.md` at repo root |
| Image layout decode | done | VA = file offset + 0x1000; sections 0x1000..0x75E000; header fields at 0x920–0x934; name list 0x984; import entries stride-20 from 0xA60 — see `docs/image-layout.md` |
| Entry points | done | `start` @ 0x426AD0, `kernel_thunk` @ 0x558660 |
| Boot chain analysis | done (analysis only) | start -> sub_425C71 -> sub_425BD9 -> game routine at 0x16C7E8 -> sub_233E0; Xbox OS glue is reference material, not a required Windows port layer; see `docs/startup-path.md` |
| Memory subsystem mapped | partial | custom debug malloc (sub_2A7B00/2A7E60/3DDF20), source-file/line tags on allocations (`D:\work\Bfd\Game\...`) |

## Next steps
1. Confirm the host scaffold runs on the Windows main thread; replace the stock ImGui demo with a project status/debug panel.
2. Investigate `dvddata/fmv/frontend/BootIntro.xmv` as the first visible startup slice. IDA confirms literals `frontend\\BootIntro.xmv` and `frontend\\BootIntro2.xmv`; `sub_1F62A0` (`0x1F62A0`) builds a root+table path and opens it through asynchronous I/O, but its role in selecting/playing the boot video is not yet proven.
3. Trace which caller/index selects the boot movie and follow its stream callbacks into XMV decode/audio/presentation; inspect the container streams and choose a Windows playback route after confirming codecs.
4. Show the intro in the existing host and support skip/finish transitions.
5. In parallel, verify the call ABI from `start` through `sub_425C71` / `sub_425BD9` and target/prototype near 0x16C7E8 before relying on those boot-chain notes.
6. After the intro slice, trace the title/menu path and its minimum assets; add other Xbox services only when that behavior requires them.

Do not prioritize a complete Xbox kernel/XDK mapping table or a literal port of the firmware fatal path. Add compatibility shims incrementally when a selected game slice needs them. The current first slice candidate is `BootIntro.xmv`; see `docs/startup-path.md` for the evidence and checklist.

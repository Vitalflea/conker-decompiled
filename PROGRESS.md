# Progress ledger

Target binary under active study: **default.xbe** (Mar 14, 2005 prototype), via IDA Pro (ida-pro-mcp).

## Done
| Item | Status | Notes |
|---|---|---|
| README & project charter | done | `README.md` at repo root |
| Image layout decode | done | VA = file offset + 0x1000; sections 0x1000..0x75E000; header fields at 0x920–0x934; name list 0x984; import entries stride-20 from 0xA60 — see `docs/image-layout.md` |
| Entry points | done | `start` @ 0x426AD0, `kernel_thunk` @ 0x558660 |
| Boot chain decompilation | done | start -> sub_425C71 (PsCreateSystemThreadEx) -> thread entry sub_425BD9 -> main routine sub_16C740 -> dispatcher sub_233E0; abort paths sub_42728E / sub_4237F2 — see `docs/startup-path.md` |
| Memory subsystem mapped | partial | custom debug malloc (sub_2A7B00/2A7E60/3DDF20), source-file/line tags on allocations (`D:\work\Bfd\Game\...`) |

## Next steps
1. Full IFSL/import enumeration for all module ids (0x0001…): sizes, checksums, section boundaries per module.
2. Identify what `sub_233E0`'s object model is (vtable layouts at +8/+12; method slots used) — this defines the class hierarchy to reconstruct first.
3. Reconstruct the smallest verifiable unit first: `start()` + wrappers (sub_4239F9/sub_4237F2/sub_4237C4/sub_42728E) into C++ mirroring Xbox SDK types, then build and diff bytes against [0x426A40..0x426AD0+...].
4. Pin down period-correct compiler (MSVC version family used by Konami 2005 Xbox builds) before declaring byte-exactness achievable end-to-end.

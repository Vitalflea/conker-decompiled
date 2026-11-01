# Startup path of default.xbe

All addresses are virtual (VA = file offset + 0x1000).

## Call graph

```
kernel_thunk 0x558660            Xbox kernel entry shim
        |
start() 0x426AD0                 "WinMain"
  |-> sub_425C71(0,0,&off_426A5C,0,0,0)         create system thread
  |     -> PsCreateSystemThreadEx(..., sub_425BD9)   kernel call
  |     -> on fail: sub_4237F2(status) -> RtlNtStatusToDosError -> sub_4237C4
  |-> sub_42728E(1,1,0)                        fatal-abort fallback
  |     -> HalReturnToFirmware(4) / sub_426776(0,ctx)
  `-> sub_4239F9(thread_handle)               NtClose(handle); 1/0
```

`off_426A5C` contains **0x16C7E8**, which lies inside `sub_16C740` — the real top-level routine of the game.

## sub_425C71 (thread creation wrapper)

```c
v6 = a2 ? a2 : MEMORY[0x10130];              // stack base
status = PsCreateSystemThreadEx(&out, 0, v6, dword_86A8E8 /*=148*/,
                               a6, a3 /*context ptr*/, a4, (a5>>2)&0xFFFFFF01, 0, sub_425BD9);
if (ok || thread-id>=2 || no-rpc-context) return out;
sub_4237F2(status);                          // log NTSTATUS as DOS error
return 0;
```

## sub_425BD9 (system-thread entry point)

* Reads `ActiveRpcHandle` from `NtCurrentTeb()`; zeroes a 0x90-byte context region at `ActiveRpcHandle[10]+4` and writes an end marker before it.
* Calls `sub_425AB4(1)` — walks a callback list (linked list rooted at `off_645F24`, guarded by critical section `off_645F08`) invoking each element's method with arg 1 ("phase enter").
* Invokes the passed-in entry function (`sub_16C740`) with 15 args.
* On return: `sub_425AB4(0)` then `PsTerminateSystemThread(result)`.

## sub_16C740 (top-level game routine, ~0x2BC bytes)

```c
byte_837B09 = 0;
obj = sub_A4B70(dword_8357C8);           // construct object type 42 (chareffects.cpp)
for (i in {3 const triples}) {
    p = *(DWORD*)(dword_835710 + 64);
    sub_3D9850();                        // PRNG tick over 249-word table
    sub_233E0(p, flag_i, -1082130432 /*0xC1000000*/, &const_i, 5, -1, 0, 0);
}
return sub_233E0(*(DWORD*)(dword_835710+64), 1, -1082130432, 0, 0, -1, 0, 0);
```

The three constant triples passed to `sub_233E0`:
```
(-983580672, -1006075904, -977303552)
(-993861632, -996720640, -994009088)
(1140359168, -997859328, -989622272)
```

## sub_233E0 (core dispatcher, ~0x6xx bytes)

Virtual-dispatch routine: picks a vtable slot from flags bit4 (offset a4+8 vs a4+12), calls a constructor-like method returning handle `v15`, then a play/execute method with 9 args including `(flags>>5)&0xFFFFFF01` and `flags&1`; scans up to 128 entries of a linked list for matching keys (fields +36/+48 compared against a10/a8); finalization via `sub_23600`, `sub_2E100`, `sub_12A50` or `sub_243C80`. Gates on globals `byte_8356F0/F1`, `byte_770105`, counter array at `dword_835710+72`.

## Memory subsystem (first allocation site)

`sub_A4B70 -> sub_D9640(42, idx) -> sub_2A7E60(size, "D:\\work\\Bfd\\Game\\Effects\\CharEffects\\chareffects.cpp", line, -1)`

* Custom debug-instrumented malloc; every allocation records its C++ source file + line (`D:\work\Bfd\Game\...` build tree).
* Heap init `sub_2A7B00`: reserves 10 MiB contiguous VA range (`MmAllocateContiguousMemoryEx(10485760, 0, 0x4000000, 0, 1028)`), builds pools at dword_838878 / dword_838874 / dword_838880, scribble-fills freed blocks (0xCC/0xBB/0xDD patterns), `__debugbreak()` on corruption.
* Per-class instance registry: `dword_764030[class_id]` stores live instances.

# Startup path and porting guidance

Target analyzed so far: `default.xbe` from the Mar 14, 2005 prototype. Addresses below are virtual addresses in the IDA database.

## What the retail startup tells us

The executable entry at `start` (`0x426AD0`) creates a system thread through `sub_425C71`, which enters through `sub_425BD9`. That wrapper eventually calls the game routine at `0x16C7E8` (inside `sub_16C740`). The top-level game routine constructs an object and dispatches several startup records through `sub_233E0`.

This is useful evidence about the game's initialization order and dependencies. It does **not** make a literal Windows port of the Xbox startup wrapper a prerequisite for a Windows port. The thread wrapper, Xbox TEB state, NTSTATUS conversion, and firmware fatal paths are platform services. Windows should own process startup, the main window, graphics device, input, and shutdown; game logic should be called from a small Windows host after its actual prerequisites are available.

## Current reverse-engineering findings

```text
start 0x426AD0
  -> sub_425C71             Xbox system-thread creation wrapper
     -> sub_425BD9          thread entry / callback phases / context setup
        -> 0x16C7E8          game startup routine (inside sub_16C740)
           -> sub_A4B70      constructs an object (class/type 42)
           -> sub_233E0      dispatches startup records
  -> sub_4239F9              closes the returned thread handle
```

The fatal path includes `sub_42728E`, `sub_426776`, and fallback code that builds Xbox launch data before handing off to firmware. Those routines are valuable for understanding crash behavior, but they are not part of the first Windows gameplay milestone.

The game routine's observed calls include a custom allocation path (`sub_A4B70` -> `sub_D9640` -> `sub_2A7E60`) and a dispatcher (`sub_233E0`) that depends on global state, object layouts, and virtual methods. That dependency chain is the meaningful porting work. It cannot be replaced by the Win32 boot wrapper or an ImGui demo and still count as game startup.

## Assessment of `bootchain.cpp` / `bootchain.h`

Treat these files as **exploratory notes expressed as code**, not a functioning or validated port. Keep them out of the critical path until individual behaviors are checked against IDA and given explicit Windows semantics.

Reasons:

- `GameThreadEntry` and `PsCreateSystemThreadEx` argument flow needs ABI validation. The current wrapper passes parameters in positions that do not obviously match the retail call shown in the comments; the claimed `0x16C7E8` entry is also declared as a generic 15-argument function without a verified prototype.
- `TebX` is a partial model, but fields used as pointers (`UniqueProcess`, `ActiveRpcHandle`) are uninitialized in the current implementation. Several routines then derive addresses from those fields. This can crash before reaching game logic.
- The callback list and fatal dispatch table have null placeholder bounds/head values. Iteration and fallback behavior are not established, so callback and error paths are not safe to execute.
- `CreateThread` plus immediate `CloseHandle` does not wait for the game thread and therefore does not reproduce the lifetime implied by startup. A Windows host should generally run its game loop on the owning thread and shut down through normal window/process control.
- `HalReturnToFirmware` cannot be represented by `ExitProcess` as a general equivalent. The fatal dump/launch-data path is Xbox-specific and should be replaced by Windows logging/error reporting if needed.
- The apparent NTSTATUS-to-DOS conversion and device-path mappings are placeholders, not validated compatibility layers.

No conclusion here says the original Xbox startup path is unimportant. It is important reference material; most of its OS glue is unnecessary to implement literally on Windows.

## Recommended plan

### Milestone 1: Make the host reliable

Keep a conventional Win32 entry point and run window creation, DX11 setup, ImGui instrumentation, message processing, and shutdown on the main thread. Replace the stock ImGui demo with a small project-owned status/debug panel. Keep DX11 and ImGui as tools for validating the host, not as evidence that game rendering or game UI has been ported.

### Milestone 2: Choose one bounded game-facing slice

Continue from the first real game routine, not from Xbox kernel wrappers. In IDA, verify the `0x16C7E8` call target/prototype and trace its immediate dependencies. Select a self-contained startup object or data parser with a manageable dependency set. Record for each candidate: address, callers/callees, globals touched, allocations, platform imports, assets required, and a simple observable result. Prefer a slice whose output can be displayed or checked without implementing the entire renderer.

### Milestone 3: Build only the required compatibility layer

For the selected slice, implement the smallest required memory, file, timing, input, or thread abstraction. Avoid recreating Xbox services that the slice does not call. Preserve original structures only where field layout is needed by the reconstructed logic; keep host handles and graphics objects in separate Windows-side structures.

### Milestone 4: Grow toward an on-screen result

Trace the title/menu startup path and its asset dependencies, then load one real game asset and reproduce one visible state. Add Xbox input, audio, and rendering compatibility as the traced feature requires them. Keep each step tied to a concrete behavior that can be observed in the reference game and the port.

## Immediate next analysis tasks

1. In IDA, inspect the call site at `start` and the full call setup in `sub_425C71` / `sub_425BD9`; record the exact calling convention, stack arguments, and which pointer is the game routine.
2. Verify the target address `0x16C7E8` independently and recover its true prototype. Do not infer its semantics from the current ImGui stub.
3. Trace `sub_16C740` through `sub_A4B70` and the first `sub_233E0` call. Enumerate required globals, constructors, allocators, and imported services before choosing a slice.
4. Identify the actual title/menu path and the minimum assets needed for a first visible game screen.
5. Retain boot-chain source only if it is useful as annotated research; either correct and test each compatibility behavior or remove it from the build until then.

## First concrete startup target: `BootIntro.xmv`

The likely opening movie has been identified at:

`iso/Conker Live & Reloaded (Mar 14, 2005 prototype)/dvddata/fmv/frontend/BootIntro.xmv`

The file is 9,875,456 bytes. Its first 16 bytes are `00 10 01 00 00 20 01 00 00 20 01 00 78 6F 62 58`; the final four bytes spell `xobX` in file order (the little-endian Xbox signature `Xbox`). This supports treating the file as Xbox media data, but does not by itself establish codec details or prove this exact file is the one selected at startup.

Recommended investigation sequence:

1. In IDA, inspect XMV-related imports/calls (the executable import metadata names XMV and WMADEC) and find the movie playback entry point. Follow its callers into startup/menu state selection.
2. Search for the movie basename and path fragments in the executable and data manifests. The cache text manifests inspected so far do not mention `BootIntro.xmv`; runtime code may build the path or use a separate FMV enumeration.
3. Record the XMV open/read/decode/audio calls, expected path convention, and playback completion/skip transitions. Identify whether the game uses XMV services directly or wraps them in its own media manager.
4. Inspect the container header and stream with an XMV-aware parser/player, separately from reverse-engineering game playback. Do not assume a generic WMA decoder covers the complete XMV video stream.
5. For a first Windows milestone, play this file in a small standalone host path and support a skip/finish transition. Keep the existing DX11/ImGui example as the app shell and instrumentation. Choose a Windows decoder only after the stream format and audio/video requirements are confirmed.

This is a focused vertical slice: locate one known startup asset, understand how the game requests it, decode it, display it, and advance when it finishes or is skipped. It avoids needing to port the full game renderer before producing a recognizable startup result.

### IDA lead: `sub_1F62A0` and the boot movie strings

The active IDA database confirms `BootIntro.xmv` is embedded as the string `frontend\\BootIntro.xmv` at `0x5EC525`; an adjacent entry at `0x5EC50D` is `frontend\\BootIntro2.xmv`. This establishes that both names are known to the executable, but not which one is selected on a given boot.

`sub_1F62A0` (`0x1F62A0`) is a concrete asynchronous file/stream setup routine:

- Reads an index from `[a1+4]` and selects `off_6AEB88[index]` (the table begins at `0x5EC51C`).
- Builds a path with `"%s\\%s"`, using `byte_838358` as the root and the selected table string as the relative path.
- Obtains/initializes an I/O request object through `sub_1F0C00`, opens the path with `sub_42386E`, and configures a buffered/asynchronous stream object.
- Registers `sub_1F6490` / `sub_1F64A0` callbacks through `sub_2517C0`.

Its known callers are `sub_1F5ED0`, `sub_1F6070`, `sub_1F6170`, and `sub_1F64A0`. `sub_1F5ED0` and `sub_1F6070` initialize a shared object with index `1`; `sub_1F6170` retries the indexed open and then changes state; `sub_1F64A0` is also used as a callback. The executable string table includes both BootIntro names, but IDA currently reports no direct data xrefs from those string addresses. The path table appears to contain additional pointer data after the two strings, so confirm its actual bounds/indexing in the disassembly before assuming its layout.

This makes `sub_1F62A0` a useful file-streaming lead, not yet the confirmed boot-video player or selection site. Next trace the callers of the stream setup, identify the numeric index used for `BootIntro.xmv`, and follow the object/callbacks into the XMV decoder and presentation path. Also establish whether startup selects `BootIntro.xmv` or `BootIntro2.xmv` based on build/state.

### Completion path from `Movie_Play` (active IDA database)

The boot stream object uses index `1`: `sub_1F5ED0` allocates/initializes it with `*object = 1`, and `MovieStream_Init` indexes `off_6AEB88[1]`. The table's second pointer resolves to `frontend\\BootIntro.xmv` at `0x5EC51C`; index `0` resolves to `frontend\\BootIntro2.xmv` at `0x5EC504`.

For the normal completion path, `MovieStream_Init` calls `Movie_Play(0, 1, 0, sub_1F6490, sub_1F64A0, path)`. `Movie_Play` runs its XMV read/decode loop and sets `dword_83833C = 2` when its stream reports terminal status `1` or `2`, then returns to `MovieStream_Init`, which returns to its caller. The callback `sub_1F6490` only resets the stream flag at `stream+44`; `sub_1F64A0` handles read completion/reinitialization. Neither callback directly selects the next front-end state.

The front-end tick `sub_1FBAC0` dispatches `funcs_1FBB5E[dword_7697DC]()` once per tick (unless the dispatcher is gated by `dword_763C6C`). The table entry at index `1` is `sub_1F6070`. In that handler, state `1` calls `sub_1F61C0`; if completion/input conditions allow it, the handler proceeds through state `2` and `sub_1F6170`. `sub_1F6170` retries/advances the boot state, and on its terminal branch sets state `3` and calls `sub_4EF20(dword_7E8928, 1, 0, 0, 0)`. When the state handler observes state `3`, it calls `sub_1F60E0(1)` and clears `dword_837ED4`; that cleanup resets boot/loading flags and frees stream-related state.

So the useful execution outline is:

```text
XMV reaches terminal status
  -> Movie_Play sets dword_83833C = 2 and returns
  -> MovieStream_Init returns through its caller
  -> front-end tick sub_1FBAC0 dispatches state handler
  -> state-1 handler sub_1F6070 / sub_1F61C0 evaluates completion and input
  -> sub_1F6170 advances to state 3 and invokes sub_4EF20(...)
  -> sub_1F60E0(1) tears down the boot stream state
```

This is conditional rather than a single straight-line return chain: `Movie_Play` is a synchronous pump in this path, while the front-end state transition happens on subsequent ticks and can take an input/skip branch. `sub_4EF20` is the next significant handoff found on the terminal branch; its caller-specific parameters/state must be examined to identify the exact destination screen/state for this build.

## Evidence and limitations

The call chain and dispatcher notes above are based on the current project notes and comments, not a fresh IDA inspection in this review. The implementation defects listed above are visible in the current source. Before implementing a game-facing slice, validate address/prototype claims against the active IDA database and target executable.

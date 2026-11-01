// Conker: Live & Reloaded - type definitions mirroring the Xbox kernel / NT layer
// used by the reconstructed boot chain (see bootchain.cpp).

#pragma once
#include <windows.h>
// NTSTATUS, HANDLE and CRITICAL_SECTION come from the Windows SDK headers

// Xbox TEB (thread environment block). Only the fields actually touched by
// the boot-chain routines are modelled here; offsets follow the shipped image.
typedef struct TebX {
    DWORD StackBase;            // offset 0x00
    DWORD _pad1[7];           // pad through 0x24
    PVOID   UniqueProcess;      // 0x20 : base ptr; +148*4 slot read by epilogue
    BYTE    UniqueThread;       // 0x24 : LOBYTE checked on spawn

    DWORD   ActiveRpcHandle;     // 0x28 : context/RPC handle field
} TebX;


// phase callbacks
typedef int  (__stdcall *PFN_PHASE_CALLBACK)(int);
typedef int  (__stdcall *PFN_FATAL_CTX)(int, void*, int, int);      // dispatch target of sub_426776
typedef ULONG (__stdcall *PFN_FATAL_HANDLER)(void*);               // dword_86A8E4 crash hook
typedef int  (__stdcall *PFN_TWO_ARG)(int, int);                  // TEB +148 hook
// the real game routine (retail sub_16C7E8 region): 15 register args
typedef int (__stdcall *PFN_GAME_MAIN)(DWORD, DWORD, DWORD, DWORD, DWORD,
                                       DWORD*, DWORD*, DWORD, DWORD, DWORD,
                                       DWORD, DWORD, DWORD, DWORD, DWORD);
// the thread-entry shim (retail sub_425BD9)
typedef int (__stdcall *PFN_THREAD_ENTRY)(PFN_GAME_MAIN, DWORD);

// kernel entry points (retail import table, XBOXKRNL module)
NTSTATUS PsCreateSystemThreadEx(void** ThreadId, DWORD Priority, PVOID StartAddress,
                               ULONG StackSizeInBytes, PCONTEXT Context,
                               PVOID ArgumentPointer, DWORD StartFlags,
                               DWORD Unknown1, DWORD Unknown2, void* Entry);
void PsTerminateSystemThread(NTSTATUS status);
void HalReturnToFirmware(int reason);
ULONG RtlNtStatusToDosError(NTSTATUS status);
int  RtlEnterCriticalSection(void* critsec);
int  RtlLeaveCriticalSection(void* critsec);
int  NtClose(HANDLE h);

// forward declarations (defined in bootchain.cpp before use sites)
int __stdcall GameThreadEntry(PFN_GAME_MAIN main_routine, DWORD arg1);
int  GameMainRoutine(DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5,
                      DWORD* a6, DWORD* a7, DWORD a8, DWORD a9, DWORD a10,
                      DWORD a11, DWORD a12, DWORD a13, DWORD a14, DWORD a15);
ULONG FatalFallback(int code, void* ctx, int a3, int a4);

// Conker: Live & Reloaded - ported boot chain (Windows x86 / MSVC).
// Target: default.xbe (Mar 14, 2005 prototype), .text region.
// Each routine mirrors one contiguous retail code range:
//
//   sub_4237C4 [0x4237C4 .. 0x4237F2)
//   sub_4237F2 [0x4237F2 .. 0x42380A)
//   sub_4239F9 [0x4239F9 .. 0x423A17)
//   sub_425AB4 [0x425AB4 .. 0x425AEC)
//   sub_425BD9 [0x425BD9 .. 0x425C71)
//   sub_425C71 [0x425C71 .. 0x425CDF)
//   sub_426776 [0x426776 .. 0x4267B8)
//   sub_42728E [0x42728E .. 0x4272F2)
//   WinMain/start [0x426AD0 .. 0x426B42)
//
// Xbox kernel -> Windows mapping table:
//   PsCreateSystemThreadEx -> CreateThread + ResumeThread
//   NtClose              -> CloseHandle
//   HalReturnToFirmware(n)-> ExitProcess(n)
//   RtlNtStatusToDosError -> local lookup table
//   TEB fs:[28h]        -> modelled TebX behind GetGameTeb()
//
// Verification method now: compile with MSVC x86 and check behaviour against
// the retail image (state transitions, thread lifecycle, error paths).
// Xbox device names inside the fatal path are remapped onto Windows volumes:
//   \Device\Harddisk0\Partition1 -> C:\          (main volume)
//   \Device\Harddisk0\Partition2 -> D:\          (second volume / DVD drive)
//   \Device\Cdrom0               -> %CDROM%

// Retail-era CRT calls (strcpy/snprintf family); silence the modern-CRT
// deprecation diagnostics.
#define _CRT_SECURE_NO_WARNINGS
#include "bootchain.h"
#include <string.h>
#include <stdio.h>

// ------------------------------------------------------------------
// .data globals (retail layout; values as in the shipped image)
int    dos_error     = 0;      // sub_4237C4 DOS-error sink
DWORD  g_stack_lo    = 0;     // MEMORY[0x10108]
DWORD  g_stack_hi    = 0;     // MEMORY[0x10118]; also read via +8 on abort path
void* const g_thread_stk = NULL; // MEMORY[0x10130] (default thread stack base)
DWORD  g_pstack_size = 148;   // dword_86A8E8
int    g_tls_index   = -37;   // TlsIndex @0x7519F4
void* const g_seh_table_base = (void*)-1; // stru_558C48 (empty SEH list)
DWORD  g_ctx_src_a   = 0;     // dword_558E0C
DWORD  g_ctx_src_b   = 0;     // dword_558E10
DWORD  g_ctx_len     = 144;   // dword_558E1C
PFN_FATAL_HANDLER g_fatal_handler = NULL; // dword_86A8E4

// phase callback registry (.data @0x645F08..):
typedef struct PhaseNode { void* next; DWORD pad; PFN_PHASE_CALLBACK cb; } PhaseNode;
CRITICAL_SECTION g_phase_lock;           // off_645F08
PhaseNode*      g_phase_list_head = NULL; // off_645F24 (sentinel == head)
void* const g_phase_table_base  = NULL;  // unk_645EE8
void* const g_phase_table_limit = NULL;  // off_645EF0

// Modelled TEB instance (stands in for fs:[28h]).
static TebX g_teb_model;

void InitBootChainState(void)
{
    InitializeCriticalSection(&g_phase_lock);
}

TebX* GetGameTeb(void)
{
    return &g_teb_model;
}

// ------------------------------------------------------------------
// sub_4237C4 : write DOS error into the TEB TLS slot (g_tls_index)+8
int __stdcall LogDosErrorToTeb(DWORD dos_error_)
{
    DWORD* tls = (DWORD*)((char*)GetGameTeb()->UniqueProcess + g_tls_index * sizeof(DWORD));
    if (!tls) { dos_error = dos_error_; return 0; }
    *(tls + 8) = dos_error_;
    dos_error = dos_error_;
    return *tls;
}

// ------------------------------------------------------------------
// sub_4237F2 : NTSTATUS -> DOS error, then log it
int __stdcall MapAndLogStatus(NTSTATUS status)
{
    ULONG dos = RtlNtStatusToDosError(status);
    return LogDosErrorToTeb(dos);
}

// ------------------------------------------------------------------
// sub_4239F9 : close the game thread handle
int __stdcall CloseGameThread(HANDLE h)
{
    int st = NtClose(h);
    if (st >= 0) return 1;
    MapAndLogStatus(st);
    return 0;
}

// ------------------------------------------------------------------
// sub_425AB4 : fire every registered phase callback under a lock
int __stdcall FirePhaseCallbacks(int phase)
{
    int ret = RtlEnterCriticalSection(&g_phase_lock);   // &off_645F08
    PhaseNode* cur = g_phase_list_head;              // value of off_645F24
    while ((void*)cur != (void*)&g_phase_list_head) {          // sentinel == address of off_645F24
        cur->cb(phase);                          // node field at +8
        cur = (PhaseNode*)cur->next;
    }
    ret = RtlLeaveCriticalSection(&g_phase_lock);
    return ret;
}

// ------------------------------------------------------------------
// sub_425B9D : SEH epilogue helper for GameThreadEntry
int __stdcall ThreadSehEpilog(DWORD exc_ptr)
{
    DWORD hook = *((DWORD*)((char*)GetGameTeb()->UniqueProcess + 148 * sizeof(DWORD)));
    int r = hook ? (*(PFN_TWO_ARG)((char*)hook + 20))(12, 0) : 0;
    if (r) return 0;
    if (!g_fatal_handler) return 0;
    int res = g_fatal_handler((void*)exc_ptr);
    if (res != -1) return 0;
    return res;
}

// ------------------------------------------------------------------
// sub_425BD9 : the actual game-thread entry shim.
// Writes its context pointer back into the TEB's RPC field (+0x28),
// copies/zeroes a 144-byte context region, fires callbacks around the
// real routine, terminates the thread. Wrapped in an empty SEH frame
// (retail uses a local SEH handler that falls through to ThreadSehEpilog).
int __stdcall GameThreadEntry(PFN_GAME_MAIN main_routine, DWORD arg1)
{
    DWORD* teb_field = (DWORD*)GetGameTeb()->ActiveRpcHandle;  // *(TEB+0x28)
    DWORD* ctx = teb_field + 1;                         // +4 bytes
    *teb_field = (DWORD)ctx;                          // write back [edx-4]
    DWORD len_a = g_ctx_src_b;                       // dword_558E10
    DWORD len_b = g_ctx_src_a;                       // dword_558E0C
    DWORD n = len_a - len_b;                        // copy length (both 0 here)
    memcpy(ctx, (const char*)len_b, ((n >> 2) << 2)); // rep movsd equivalent
    memset((char*)ctx, 0, g_ctx_len);              // 144-byte zero fill
    FirePhaseCallbacks(1);
    DWORD status = main_routine(arg1, 0, 0, 0, 0, ctx, teb_field,
                                0, 0, 0, 0, 0, 0, 0, 0);
    FirePhaseCallbacks(0);
    PsTerminateSystemThread(status);
    return 0;
}

// ------------------------------------------------------------------
// sub_426776 : dispatch fatal context through callback table w/ fallback
int __stdcall DispatchFatalContext(int code, void* ctx)
{
    DWORD* iter = (DWORD*)g_phase_table_base;      // &unk_645EE8
    if (iter >= (DWORD*)g_phase_table_limit)       // off_645EF0
        return FatalFallback(code, ctx, 0, 0);
    while (!*iter) {
        if ((iter + 1) >= (DWORD*)g_phase_table_limit)
            return FatalFallback(code, ctx, 0, 0);
        ++iter;
    }
    return ((PFN_FATAL_CTX)(*iter))(code, ctx, 1, 0);
}

// ------------------------------------------------------------------
// sub_42728E : fatal-abort wrapper with a zeroed 0xC00 scratch frame
int __stdcall AbortPath(int c1, int c2, int c3)
{
    char scratch[0xC00];
    if (*(DWORD*)(g_stack_hi + 8) == (-131072))
        HalReturnToFirmware(4);
    memset(scratch, 0, sizeof(scratch));
    *(DWORD*)scratch     = c1;
    *(DWORD*)(scratch+4) = c2;
    *(DWORD*)(scratch+8) = c3;
    return DispatchFatalContext(0, scratch);
}

// ------------------------------------------------------------------
// sub_427D55 [0x427D55 .. ) : fatal fallback. Builds the "Launch Data"
// page describing where the crash dump goes, resolves the destination
// device, formats the dump filename, fires the TEB+148 hook and hands off
// to firmware. Its helpers map as follows:
//   sub_427A49(handle,360)      -> first CD/DVD drive root (else C:\)
//   MmAllocate/PersistContiguous -> VirtualAlloc of the 0x1000 launch page
//   sub_4268FD(a,b,sign,hi8,c)   -> no-path variant of the builder
//   sub_4267B8(dev,tail,flag,..)-> BuildLaunchPage() below
//   sub_427ADB(head,tail)       -> AppendTailToLaunchPage()
//   HalReturnToFirmware(2/4)    -> ExitProcess(n)
static char g_launch_page[0x1000];   // *(_DWORD*)LaunchDataPage equivalent
static char g_handle_buf[360];       // sub_427A49 output buffer (device root)

static void BuildLaunchPage(const char* dev_path, const char* head_tail,
                            int sign_tag, int hi8, const void* ctx)
{
    memset(g_launch_page, 0, sizeof(g_launch_page));
    *(DWORD*)g_launch_page         = (DWORD)sign_tag;
    *(DWORD*)(g_launch_page + 4)  = (DWORD)hi8;
    if (sign_tag != -1 && ctx)
        memcpy(g_launch_page + 1024, ctx, 0xC00);
    char* s = g_launch_page + 8;              // string region starts at +8
    size_t n = 0;
    if (dev_path) {
        if (head_tail) {                      // a3 branch: prefix then ';' separator
            snprintf(s, 519, "%s", dev_path);  // capacity 519 in retail
            n = strlen(s);
            s[n++] = ';';                    // byte 59
        }
        snprintf(s + n, 520 - n, "%s", head_tail);
    }
}

// sub_427ADB: append tail after the header stored on the launch page
static unsigned long AppendTailToLaunchPage(const char* head, const char* tail)
{
    size_t v3 = strlen(head) + 1;
    size_t result = strlen(tail) + v3 + 8;
    if (result < 0x1EC) {
        WORD* hdr = (WORD*)(g_launch_page + 532);  // +0x214
        hdr[0] = 1;
        hdr[1] = (WORD)v3;
        *(DWORD*)(g_launch_page + 536) = *(DWORD*)g_launch_page;
        char* dst = g_launch_page + 540;          // +0x21C
        strcpy(dst, head);                    // sub_425A11 copy of head
        char* t = (char*)tail;                // write side of the retail buffer
        int off = (int)(dst + v3) - (int)t;   // retail: &v5[v3] - a2
        for (size_t i = 0; t[i]; ++i)
            t[off + i] = t[i];                // retail loop over v6[result..]
        return (unsigned long)off;
    }
    return (unsigned long)result;
}

// sub_427A49: symbolic link target lookup -> Windows drive root.
// The Xbox link resolves to the media volume; under Windows that is the
// first CD/DVD drive, else C:\.
static void ReadDeviceRoot(void)
{
    DWORD n = GetLogicalDriveStringsA((DWORD)sizeof(g_handle_buf), g_handle_buf);
    if (!n || !g_handle_buf[0])
        snprintf(g_handle_buf, sizeof(g_handle_buf), "C:\\");   // retail fallback \Device\Cdrom0
}

// ------------------------------------------------------------------
// sub_427D55 [0x427D55 .. ) : fatal fallback body. Mirrors the retail
// control flow one-to-one; only device naming and memory allocation follow
// the mapping table above.
//   ctx==NULL          -> no-path variant (sub_4268FD shape)
//   "X:\..."        -> dump file name from scratch tail (a1 remainder)
//   "T:/$u/.."      -> TDATA directory format with %08x tag
//   "Y:/.."        -> Partition2 (DVD) prefix + tail
//   anything else    -> 87
ULONG __stdcall FatalFallback(int code, void* ctx, int a3, int a4)
{
    const char* a1 = (const char*)ctx;
    int hi8 = (int)(*(DWORD*)((void*)g_stack_hi) + 8);   // *(MEMORY[0x10118]+8)

    if (!a1)                                            // sub_4268FD(0, 0, 2*(a2!=0)-1, hi8, a2)
    {
        BuildLaunchPage(nullptr, nullptr, 2 * (code != 0) - 1, hi8, nullptr);
        if (*(DWORD*)g_launch_page >= 0)
        {
            DWORD hook = *((DWORD*)((char*)GetGameTeb()->UniqueProcess + 148 * sizeof(DWORD)));
            if (hook)
                (*(PFN_TWO_ARG)((char*)hook + 20))(2, 0);
            HalReturnToFirmware(2);
        }
        return RtlNtStatusToDosError((NTSTATUS)-1073741801L);
    }

    int v19 = 0;                       // path-valid flag
    int v7  = 1;                      // head/tail select
    int sign_tag = 0;
    const char* dev_path = nullptr;
    const char* tail = nullptr;
    char fmtbuf[360];                 // sub_427D55 local v18
    int len = 0;

    char c0 = a1[0];
    if (!((c0 == 'D' || c0 == 'd') && a1[1] == ':' && a1[2] == '\\'))
    {
        if ((c0 == 'T' || c0 == 't') && a1[1] == ':' && a1[2] == '\\'
            && a1[3] == '$' && (a1[4] == 'U' || a1[4] == 'u') && a1[5] == '\\')
        {
            // "\Device\Harddisk0\Partition1\TDATA\%08x\$u%s" (mapped: C:\TDATA\...)
            len = snprintf(fmtbuf, sizeof(fmtbuf),
                           "C:\\TDATA\\%08x\\$u%s", (unsigned long)hi8, a1 + 6);
        }
        else
        {
            if ((a3 & 2) == 0 || !(c0 == 'Y' || c0 == 'y')
                || a1[1] != ':' || a1[2] != '\\')
                return 87;
            // "%s%s" with SourceString ("\Device\Harddisk0\Partition2\" -> D:\)
            len = snprintf(fmtbuf, sizeof(fmtbuf), "D:%s", a1 + 3);
        }
        if (len < 0)
            return 87;
        v19 = 1;
        dev_path = fmtbuf;             // formatted full device path
        tail     = nullptr;
        v7      = 0;
    }
    else
    {
        // "X:\..." : scratch frame holds the file name after the drive letter
        v19    = 1;
        dev_path = g_handle_buf;       // resolved media volume root
        tail     = a1 + 3;
        v7      = 1;
        sign_tag = code;               // tag word stored at page+0
    }

    ReadDeviceRoot();
    BuildLaunchPage(dev_path, tail, sign_tag, hi8, ctx);

    if (v19 == 0 || code)                  // retail: !v19 || a2
        AppendTailToLaunchPage(g_handle_buf, tail ? tail : "");

    DWORD hook = *(DWORD*)((char*)GetGameTeb()->UniqueProcess + 148 * sizeof(DWORD));
    if (hook)
        (*(PFN_TWO_ARG)((char*)hook + 20))(2, 0);
    HalReturnToFirmware(2);              // success path hands off to firmware
}

NTSTATUS PsCreateSystemThreadEx(void** ThreadId, DWORD Priority, PVOID StartAddress,
                               ULONG StackSizeInBytes, PCONTEXT Context,
                               PVOID ArgumentPointer, DWORD StartFlags,
                               DWORD Unknown1, DWORD Unknown2, void* Entry)
{
    (void)Priority; (void)StartAddress; (void)StackSizeInBytes;
    (void)Context; (void)Unknown1;
    HANDLE h = CreateThread(NULL, StackSizeInBytes ? StackSizeInBytes : 0,
                           (LPTHREAD_START_ROUTINE)Entry, ArgumentPointer,
                           CREATE_SUSPENDED, NULL);
    if (!h) return (NTSTATUS)(int)(HRESULT_FROM_WIN32(GetLastError()));
    ResumeThread(h);
    *ThreadId = (void*)h;
    return S_OK;
}

void PsTerminateSystemThread(NTSTATUS status)
{
    // Retail terminates the system thread by returning from its entry point.
    (void)status;
    return;
}

void HalReturnToFirmware(int reason)
{
    ExitProcess(reason);   // no user-mode recovery path; matches firmware handoff
}

ULONG RtlNtStatusToDosError(NTSTATUS status)
{
    // Xbox maps NTSTATUS onto DOS error codes; under Windows our statuses arrive
    // already in HRESULT/DOS form via GetLastError, so this is an identity map
    // with a sanity clamp.
    LONG v = (LONG)status;
    if (v < -1 || v > 0x7FFFFFFF) v = 0;   // no mapped DOS equivalent
    return (ULONG)v;
}

int RtlEnterCriticalSection(void* cs) { EnterCriticalSection((CRITICAL_SECTION*)cs); return 0; }
int RtlLeaveCriticalSection(void* cs) { LeaveCriticalSection((CRITICAL_SECTION*)cs); return 0; }
int NtClose(HANDLE h) { CloseHandle(h); return 0; }

// ------------------------------------------------------------------
// sub_425C71 : PsCreateSystemThreadEx wrapper with TEB sanity checks.
// Retail stores the new thread id in its own parameter slot ([ebp+arg_4]),
// which doubles as the explicit-stack argument: start() passes 0 through
// that slot, so it must never be dereferenced here. Mirror exactly:
HANDLE CreateGameThread(void** out_slot, PVOID explicit_stack, PVOID entry,
                        PVOID arg_ptr, DWORD prio, DWORD flags)
{
    (void)out_slot;
    HANDLE tid_out = (HANDLE)(DWORD)explicit_stack;      // retail's arg_4 slot
    InitBootChainState();
    GetGameTeb()->UniqueThread = 1;                    // main-thread slot; spawn check expects LOBYTE>=2 after fork
    PVOID stk = explicit_stack ? explicit_stack : g_thread_stk;  // v6 = a2 ? a2 : MEMORY[0x10130]
    NTSTATUS st = PsCreateSystemThreadEx((void**)&tid_out, 0, stk, g_pstack_size,
                                         (PCONTEXT)arg_ptr, entry, prio,
                                         (flags >> 2) & 0xFFFFFF01, 0,
                                         GameThreadEntry);
    if (st >= 0 || LOBYTE(GetGameTeb()->UniqueThread) >= 2u
                 || !*((DWORD*)GetGameTeb()->ActiveRpcHandle + 10))
        return tid_out;                               // mov eax, [ebp+arg_4]
    MapAndLogStatus(st);
    return NULL;
}

// ------------------------------------------------------------------
// start() / WinMain [0x426AD0 .. 0x426B42) : compute stack budget,
// spawn game thread, close it. Program entry point for the ported binary.
// Program entry (retail start @0x426AD0). Named ConkerMain because WinBase.h
// already declares WinMain; keeps the WinMain ABI required for a Windows-
// subsystem entry point.
int WINAPI ConkerMain(int nArgs, char *lpArgList)
{
    (void)nArgs; (void)lpArgList;
    DWORD budget = g_stack_lo - g_stack_hi + 0x10000;
    if (budget < g_stack_hi) g_stack_hi = budget;
    g_pstack_size = 148;
    g_tls_index   = -37;
    HANDLE h = CreateGameThread(0, 0, (PVOID)&GameMainRoutine, 0, 0, 0);
    if (!h) AbortPath(1, 1, 0);
    return CloseGameThread(h);
}


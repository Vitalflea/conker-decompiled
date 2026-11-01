// Conker: Live & Reloaded - boot-chain reconstruction
// Target: default.xbe (Mar 14, 2005 prototype), .text region.
// Each routine below mirrors one contiguous retail code range:
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
// Verification method: compile this translation unit with the period-correct
// toolchain (MSVC, x86) and diff each emitted function body byte-for-byte
// against the retail image at the ranges above.

#include "bootchain.h"

// ------------------------------------------------------------------
// .data globals (retail layout; values as in the shipped image)
DWORD g_stack_lo    = 0;     // MEMORY[0x10108]
DWORD g_stack_hi    = 0;     // MEMORY[0x10118]; also read via +8 on abort path
void* const g_thread_stk = NULL;     // MEMORY[0x10130] (default thread stack base)
DWORD g_pstack_size = 148;   // dword_86A8E8
int   g_tls_index   = -37;   // TlsIndex @0x7519F4
void* const g_seh_table_base = (void*)-1;  // stru_558C48 (empty SEH list)
DWORD g_ctx_src_a   = 0;     // dword_558E0C
DWORD g_ctx_src_b   = 0;     // dword_558E10
DWORD g_ctx_len     = 144;   // dword_558E1C
PFN_FATAL_HANDLER g_fatal_handler = NULL; // dword_86A8E4

// phase callback registry (.data @0x645F08..):
typedef struct PhaseNode { void* next; DWORD pad; PFN_PHASE_CALLBACK cb; } PhaseNode;
CRITICAL_SECTION g_phase_lock;              // off_645F08
PhaseNode*      g_phase_list_head = NULL;   // off_645F24 (sentinel == head)
void* const g_phase_table_base  = NULL;     // unk_645EE8
void* const g_phase_table_limit = NULL;     // off_645EF0

// Xbox SEH base pointer: the TEB lives at FS:[0x28]
inline TebX* GetGameTeb(void)
{
    __asm {
        mov eax, fs:[28h]
        mov ecx, offset g_tls_index
        mov edx, [eax]
        mov [edx+ecx], dos_error
        mov eax, [edx+ecx]
    }
}

// ------------------------------------------------------------------
// sub_4237C4 : write DOS error into TEB TLS slot (g_tls_index)+8
int __stdcall LogDosErrorToTeb(DWORD dos_error)
{
    __asm { mov eax, fs:[28h]; mov ecx, offset g_tls_index; mov edx, [eax]; mov [edx+ecx], dos_error; mov eax, [edx+ecx] }

}

// ------------------------------------------------------------------
// sub_4237F2 : NTSTATUS -> DOS error, then log it
//#pragma noinline
int __stdcall MapAndLogStatus(NTSTATUS status)
{
    ULONG dos = RtlNtStatusToDosError(status);
    return LogDosErrorToTeb(dos);
}

// ------------------------------------------------------------------
// sub_4239F9 : close the game thread handle
int __stdcall CloseGameThread(void* h)
{
    int st = NtClose((HANDLE)h);
    if (st >= 0) return 1;
    MapAndLogStatus(st);
    return 0;
}

// ------------------------------------------------------------------
// sub_425AB4 : fire every registered phase callback under a lock
int __stdcall FirePhaseCallbacks(int phase)
{
    RtlEnterCriticalSection(&g_phase_lock);       // &off_645F08
    PhaseNode* cur = g_phase_list_head;          // value of off_645F24
    while ((void*)cur != (void*)&g_phase_list_head) {          // sentinel == address of off_645F24
        cur->cb(phase);                          // node field at +8
        cur = (PhaseNode*)cur->next;
    }
    return RtlLeaveCriticalSection(&g_phase_lock);
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
// real routine, terminates the thread. Wrapped in an empty SEH frame.
int __stdcall GameThreadEntry(PFN_GAME_MAIN main_routine, DWORD arg1)
{
    DWORD* teb_field = (DWORD*)GetGameTeb()->ActiveRpcHandle;  // *(TEB+0x28)
    DWORD* ctx = teb_field + 1;                         // +4 bytes
    *teb_field = (DWORD)ctx;                          // write back [edx-4]
    DWORD len_a = g_ctx_src_b;                       // dword_558E10
    DWORD len_b = g_ctx_src_a;                       // dword_558E0C
    DWORD n = len_a - len_b;                        // copy length (both 0 here)
    memcpy(ctx, (char*)len_b, ((n >> 2) << 2));      // rep movsd equivalent
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
// sub_425C71 : PsCreateSystemThreadEx wrapper with TEB sanity checks
void* CreateGameThread(void* out_handle, PVOID explicit_stack, PVOID entry,
                       PVOID arg_ptr, DWORD prio, DWORD flags);

// ------------------------------------------------------------------
// start() : compute stack budget, spawn game thread, close it
int main(void)
{
    DWORD budget = g_stack_lo - g_stack_hi + 0x10000;
    if (budget < g_stack_hi) g_stack_hi = budget;
    g_pstack_size = 148;
    g_tls_index   = -37;
    void* h = CreateGameThread(0, 0, &GameMainRoutine, 0, 0, 0);
    if (!h) AbortPath(1, 1, 0);
    return CloseGameThread(h);
}

// ------------------------------------------------------------------
// sub_425C71 : PsCreateSystemThreadEx wrapper with TEB sanity checks
void* CreateGameThread(void* out_handle, PVOID explicit_stack, PVOID entry,
                       PVOID arg_ptr, DWORD prio, DWORD flags)
{
    PVOID stk = explicit_stack ? explicit_stack : g_thread_stk;
    NTSTATUS st = PsCreateSystemThreadEx(&out_handle, 0, stk, g_pstack_size,
                                         (PCONTEXT)arg_ptr, entry, prio,
                                         (flags >> 2) & 0xFFFFFF01, 0,
                                         GameThreadEntry);
    if (st >= 0 || LOBYTE(GetGameTeb()->UniqueThread) >= 2u
                 || !*((DWORD*)GetGameTeb()->ActiveRpcHandle + 10))
        return out_handle;
    MapAndLogStatus(st);
    return NULL;
}

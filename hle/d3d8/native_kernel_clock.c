/* Street 2 PC kernel clock.
 *
 * KeTickCount (ordinal 156) is a data export the title reads directly. In the
 * shared runtime it is advanced by the NV2A polling/ack thread, which the
 * native memory variant (native_memory.cmake) never starts, so the clock stayed
 * frozen at its init value. XAPI's timeSetEvent clone (0x24ED9D), driven by the
 * engine's 60 Hz scheduler at 0x223E1A, measures its next due time against it:
 * with a frozen "now" each reschedule waited 16 ms longer than the last and the
 * engine tick at 0x34D570 advanced ~52 times in 21.7 s instead of ~1300.
 *
 * This thread only keeps the host millisecond clock visible to the guest; it
 * touches no device state. */
#include "hle_game.h"
#include <stdint.h>
#include <stdio.h>
#include "hle_win32.h"
#include "recomp/gen/recomp_types.h"
#ifdef _WIN32
#include <mmsystem.h>
#endif

#if HLE_GAME == HLE_GAME_MKA
#include "kernel/xbox_memory_layout.h"
#define KTICK_VA (XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
#else /* street2 */
#define KTICK_VA (0x00740000u + 0x020u)   /* XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT */
#endif /* HLE_GAME */

static DWORD WINAPI native_kernel_clock_thread(LPVOID unused)
{
    (void)unused;
#ifdef _WIN32
    timeBeginPeriod(1);   /* 1 ms Sleep granularity; POSIX sleeps are already fine-grained */
#endif
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        MEM32(KTICK_VA) = GetTickCount();
        Sleep(1);
    }
}

void native_kernel_clock_start(void)
{
    HANDLE h;
    MEM32(KTICK_VA) = GetTickCount();
    h = CreateThread(NULL, 0, native_kernel_clock_thread, NULL, 0, NULL);
    if (!h) {
        fprintf(stderr, "[PORT] kernel clock thread failed (error %lu)\n", GetLastError());
        return;
    }
    CloseHandle(h);
    fprintf(stderr, "[PORT] native KeTickCount clock: 1 ms host thread\n");
}

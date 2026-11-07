/* D3D8 library functions present in the XBE but never called by Mortal Kombat
 * Armageddon (no call sites), so the recompiler emitted no body for them. The
 * HLE layer still names their original bodies as sub_X_gen; reaching one means
 * the dead-code assumption was wrong, so say so loudly. */
#include <stdio.h>
#include <stdlib.h>

static void dead_gen(const char *name)
{
    fprintf(stderr, "[HLE] original body of %s reached but never recompiled (marked dead)\n", name);
    fflush(stderr);
    abort();
}

void sub_0040B0C0_gen(void) { dead_gen("D3DDevice_InsertFence 0040B0C0"); }
void sub_004119E0_gen(void) { dead_gen("D3DDevice_CreateIndexBuffer2 004119E0"); }

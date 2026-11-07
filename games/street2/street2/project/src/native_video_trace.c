/* Read-only state trace of the original video update; no forced returns. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "hle_win32.h"
#include "recomp/gen/recomp_types.h"
void sub_0014F730_gen(void);
void sub_0014F730(void)
{
    uint32_t object=g_ecx;
    sub_0014F730_gen();
    static int enabled=-1;
    static unsigned calls;
    if(enabled<0) enabled=getenv("RECOMP_VIDEO_TRACE")!=NULL;
    if(!enabled) return;
    if(++calls<=12 || calls%120==0) {
        uint32_t player=MEM32(object+0x0C);
        fprintf(stderr,"[VIDEO-TRACE] update=%u object=%08X frame=%08X aux=%08X player=%08X flags=%02X/%02X/%02X/%02X/%02X result=%08X",
                calls,object,MEM32(object+4),MEM32(object+8),player,
                MEM8(object+0xFC),MEM8(object+0xFD),MEM8(object+0xFE),MEM8(object+0xFF),MEM8(object+0x100),g_eax);
        if(player) {
            fprintf(stderr," player-data=");
            for(unsigned i=0;i<24;i++) fprintf(stderr,"%08X%s",MEM32(player+i*4),i==23?"":"/");
        }
        fprintf(stderr," host-ms=%llu engine-ticks=%u engine-hz=%u",(unsigned long long)GetTickCount64(),MEM32(0x34D570),MEM32(0x34D56C));
        fputc('\n',stderr);
        fflush(stderr);
    }
}

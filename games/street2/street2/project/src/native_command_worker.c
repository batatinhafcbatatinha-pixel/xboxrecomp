/* Street 2 command worker, reconstructed as one complete CFG.
 * XBE VA 00227853..002279A0. The old split sent branches 00227983/0022799D
 * to empty stubs, dropping the wait and the shared epilogue (saved EDI/EBX).
 * Table below is derived from 31 original dwords at VA 002279A1.
 */
#include <stdint.h>
#include <stddef.h>
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"
/* The generated indirect-call macro names the architectural EAX alias. */
#define eax g_eax
typedef struct { uint32_t address, return_address; void (*function)(void); } WorkerHandler;
static const WorkerHandler handlers[31] = {
    {0x22B692u, 0x2278B6u, sub_0022B692},
    {0x22AFE7u, 0x2278C0u, sub_0022AFE7},
    {0x22B054u, 0x2278CAu, sub_0022B054},
    {0x22B864u, 0x2278D4u, sub_0022B864},
    {0x22BD2Eu, 0x2278DEu, sub_0022BD2E},
    {0x22C163u, 0x2278E8u, sub_0022C163},
    {0x22C7E5u, 0x2278EFu, sub_0022C7E5},
    {0x22CBDEu, 0x2278F6u, sub_0022CBDE},
    {0, 0, NULL},
    {0, 0, NULL},
    {0, 0, NULL},
    {0x22A2AAu, 0x227904u, sub_0022A2AA},
    {0x22A327u, 0x22790Bu, sub_0022A327},
    {0x22A43Cu, 0x227912u, sub_0022A43C},
    {0x22A4D9u, 0x227919u, sub_0022A4D9},
    {0x22AA4Cu, 0x227920u, sub_0022AA4C},
    {0x22AAFFu, 0x227927u, sub_0022AAFF},
    {0x22AB84u, 0x22792Eu, sub_0022AB84},
    {0x22AC2Eu, 0x227935u, sub_0022AC2E},
    {0x22AD7Du, 0x22793Cu, sub_0022AD7D},
    {0x22A75Fu, 0x227943u, sub_0022A75F},
    {0x22A65Bu, 0x22794Au, sub_0022A65B},
    {0x22A552u, 0x227951u, sub_0022A552},
    {0x22AC99u, 0x227958u, sub_0022AC99},
    {0x22ACEDu, 0x227966u, sub_0022ACED},
    {0, 0, NULL},
    {0x22AE55u, 0x22795Fu, sub_0022AE55},
    {0, 0, NULL},
    {0, 0, NULL},
    {0, 0, NULL},
    {0x22B219u, 0x2278FDu, sub_0022B219}
};

static void worker_virtual(uint32_t object, uint32_t slot, uint32_t ret, uint32_t site, int arg)
{
    g_ecx=object;
    g_eax=MEM32(g_ecx);
    uint32_t saved_esp=g_esp;
    if(arg>=0) PUSH32(g_esp,(uint32_t)arg);
    uint32_t target=MEM32(g_eax+slot);
    PUSH32(g_esp,ret);
    RECOMP_ICALL_SAFE_AT(target,saved_esp,site);
}

void sub_00227853(void)
{
    PUSH32(g_esp,g_ebx);
    g_ebx=0;
    if(MEM8(0x34FF0C)!=0) goto finished;
    PUSH32(g_esp,g_edi);
    do {
        if(MEM32(0x34FF10)==g_ebx && MEM32(0x3230DC)!=g_ebx) {
            worker_virtual(MEM32(0x34FF08),0x0C,0x227886,0x227883,-1);
            g_eax=0;
            g_ecx=0x23;
            g_edi=0x3505F8;
            int32_t step=RECOMP_DF_STEP(4);
            for(uint32_t i=0;i<g_ecx;i++) MEM32(g_edi+i*step)=g_eax;
            g_edi+=g_ecx*step;
            g_ecx=0;
            g_eax=MEM32(0x3230DC);
            MEM32(0x34FF10)=g_ebx;
            g_eax=MEM32(g_eax+8)-1;
            if(g_eax<=0x1E) {
                const WorkerHandler *h=&handlers[g_eax];
                if(h->function) {
                    PUSH32(g_esp,h->return_address);
                    RECOMP_ABI_CALL(h->address,h->function);
                }
            }
            if(MEM32(0x3505FC)!=g_ebx) MEM32(0x34FF10)=0x3505F8;
            worker_virtual(MEM32(0x34FF08),0x10,0x227983,0x227980,-1);
        }
        worker_virtual(MEM32(0x34FF04),0x18,0x227990,0x22798D,5);
    } while(MEM8(0x34FF0C)==(uint8_t)g_ebx);
    POP32(g_esp,g_edi);
finished:
    g_eax=0;
    POP32(g_esp,g_ebx);
    g_esp+=4;
}

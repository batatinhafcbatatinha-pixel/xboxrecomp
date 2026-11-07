/* Native replacement of Street 2's immediate engine batches.
 * Evidence: XBE 5fabb5b7...; text.asm
 *   textured:   begin 00197F50, end 00197B50, continue 00197BD0, append 00198040/00198150
 *               (active +0x5C, count +0xA8; position 12 + color 4 + UV 8 = 24 bytes)
 *   untextured: begin 00197D90, end 001978A0, continue 00197920, append 00197E70
 *               (active +0x58, count +0xA4; position XYZW 16 + color 4 = 20 bytes)
 * Append receives CPU vertex pointers. Never serializes or decodes GPU packets.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"

extern void sub_00197F50_gen(void);
extern void sub_00197D90_gen(void);
void hle_draw_engine_batch(uint32_t primitive, const uint8_t *vertices, uint32_t count, uint32_t stride);
void hle_port_native_missing(const char *, unsigned);

#define BATCH_CAPACITY 96
#define BATCH_CAPACITY_VA 0x319748u

typedef struct {
    const char *name;
    uint32_t active_off, count_off, stride;
} BatchLayout;
static const BatchLayout TEXTURED   = { "textured",   0x5C, 0xA8, 24 };
static const BatchLayout UNTEXTURED = { "untextured", 0x58, 0xA4, 20 };

typedef struct { const BatchLayout *layout; uint32_t owner, count; uint8_t vertices[BATCH_CAPACITY*24]; } NativeBatch;
static NativeBatch batches[32];

static NativeBatch *get_batch(const BatchLayout *l, uint32_t owner)
{
    NativeBatch *free_slot = NULL;
    for (unsigned i=0; i<32; i++) {
        if (batches[i].owner==owner && batches[i].layout==l) return &batches[i];
        if (!batches[i].count && !free_slot) free_slot=&batches[i];
    }
    if (!free_slot) hle_port_native_missing("native batch owner capacity", MEM32(g_esp));
    free_slot->layout=l;
    free_slot->owner=owner;
    free_slot->count=0;
    return free_slot;
}

static void flush(const BatchLayout *l, uint32_t owner)
{
    NativeBatch *b=get_batch(l, owner);
    uint32_t count=MEM32(owner+l->count_off);
    if (count!=b->count) hle_port_native_missing("unported producer wrote native batch", MEM32(g_esp));
    if (count) {
        static unsigned reported;
        if(reported++<8) fprintf(stderr,"[ENGINE-NATIVE] %s batch primitive=%u vertices=%u owner=%08X\n",l->name,MEM32(owner),count,owner);
        hle_draw_engine_batch(MEM32(owner), b->vertices, count, l->stride);
    }
    b->count=0;
    MEM32(owner+l->count_off)=0;
}

/* Final flush clears the active flag; continuation flush keeps it. */
static void end_batch(const BatchLayout *l)
{
    uint32_t owner=g_ecx;
    MEM8(owner+l->active_off)=0;
    flush(l, owner);
    g_esp+=4;
}
static void continue_batch(const BatchLayout *l)
{
    uint32_t owner=g_ecx;
    if(MEM8(owner+l->active_off)) flush(l, owner);
    g_esp+=4;
}
/* Material, shader, texture and transform setup stay in the CPU routine. */
static void begin_batch(const BatchLayout *l, void (*original)(void))
{
    uint32_t owner=g_ecx;
    NativeBatch *b=get_batch(l, owner);
    if(b->count) hle_port_native_missing("begin with unfinished native batch", MEM32(g_esp));
    original();
    b->count=0;
}

static uint8_t *reserve_vertex(const BatchLayout *l, uint32_t owner)
{
    NativeBatch *b=get_batch(l, owner);
    uint32_t capacity=MEM32(BATCH_CAPACITY_VA);
    if(!capacity || capacity>BATCH_CAPACITY || !MEM8(owner+l->active_off))
        hle_port_native_missing("unexpected native batch contract", MEM32(g_esp));
    if(b->count!=MEM32(owner+l->count_off)) hle_port_native_missing("native vertex count mismatch",MEM32(g_esp));
    if(b->count==capacity) flush(l, owner);
    return b->vertices + b->count*l->stride;   /* packed at the layout stride */
}
static void commit_vertex(const BatchLayout *l, uint32_t owner)
{
    NativeBatch *b=get_batch(l, owner);
    b->count++;
    MEM32(owner+l->count_off)=b->count;
    g_eax=b->count;
}

/* this=ecx; (position*, color*, uv*); ret 0x0C */
static void append_textured(void)
{
    uint32_t owner=g_ecx, pos=MEM32(g_esp+4), color=MEM32(g_esp+8), uv=MEM32(g_esp+12);
    uint8_t *v=reserve_vertex(&TEXTURED, owner);
    memcpy(v, (const void *)XBOX_PTR(pos), 12);
    memcpy(v+12, (const void *)XBOX_PTR(color), 4);
    memcpy(v+16, (const void *)XBOX_PTR(uv), 8);
    commit_vertex(&TEXTURED, owner);
    g_esp+=16;
}

/* this=ecx; (position XYZW*, color*); ret 8 */
static void append_untextured(void)
{
    uint32_t owner=g_ecx, pos=MEM32(g_esp+4), color=MEM32(g_esp+8);
    uint8_t *v=reserve_vertex(&UNTEXTURED, owner);
    memcpy(v, (const void *)XBOX_PTR(pos), 16);
    memcpy(v+16, (const void *)XBOX_PTR(color), 4);
    commit_vertex(&UNTEXTURED, owner);
    g_esp+=12;
}

void sub_00197B50(void) { end_batch(&TEXTURED); }
void sub_00197BD0(void) { continue_batch(&TEXTURED); }
void sub_00197F50(void) { begin_batch(&TEXTURED, sub_00197F50_gen); }
void sub_00198040(void) { append_textured(); }
void sub_00198150(void) { append_textured(); }

void sub_001978A0(void) { end_batch(&UNTEXTURED); }
void sub_00197920(void) { continue_batch(&UNTEXTURED); }
void sub_00197D90(void) { begin_batch(&UNTEXTURED, sub_00197D90_gen); }
void sub_00197E70(void) { append_untextured(); }

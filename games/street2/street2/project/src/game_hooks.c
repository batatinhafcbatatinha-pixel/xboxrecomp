/* Street 2 engine hooks are implemented in native_engine_batch.c and
 * native_command_worker.c, routed by hle/game_hooks.json and CMake.
 * Street 1 frontend addresses must not be transplanted.
 * PC settings: --settings, F1 or Back+Start, or Back where the game's
 * screens show the "PC Settings" hint (hle_options.c).
 */
#include <stdio.h>
#include "recomp/gen/recomp_types.h"

int guest_cpu_native_enter(void);
void guest_cpu_native_leave(int token);

/* Front-end screen loads: 0x1793E0(const char *name) builds "<name>.big"
 * (its "na" check and "%s.big" sit at 0x2D86FC/0x2D8700), the
 * counterpart of Street 1's 0xA2BB0. */
void sub_001793E0_gen(void);
void game_screen_loaded(const char *name);

static void screen_load_hook(void);

/* Motion blur: the post-FX state (0x3404B8) pushes its motion blur to the
 * renderer in 0x14DC10(this = state): [this+0x58] on/off, +0x54 amount,
 * +0x59/+0x5C its second part, into [[this+0xC]+0x2BA/+0x140/+0x2BC..]
 * (0x14AFA0 sets the blur passes up). With the PC setting off the game's own
 * "off" path runs: the flag is cleared before the call; the script effects
 * write it again on their next update. Not verified in game. */
void sub_0014DC10_gen(void);
int hle_options_motion_blur(void);

void sub_0014DC10(void)
{
    if (g_ecx && !hle_options_motion_blur())
        MEM8(g_ecx + 0x58) = 0;
    sub_0014DC10_gen();
}

void sub_001793E0(void)
{
    int smp = guest_cpu_native_enter();
    screen_load_hook();
    guest_cpu_native_leave(smp);
}

static void screen_load_hook(void)
{
    char name[48];
    uint32_t p = MEM32(g_esp + 4);
    int i;
    for (i = 0; i < 47 && p && MEM8(p + (uint32_t)i); i++)
        name[i] = (char)MEM8(p + (uint32_t)i);
    name[i] = 0;
    sub_001793E0_gen();
    game_screen_loaded(name);
}

/* ---------------------------------------------------------------------------
 * Skinning palette blend, 0x1A4BF0 (cdecl: weights, count, dest1, bytes1,
 * dest2, bytes2, palette). For each skinned matrix it blends up to three bone
 * matrices of the palette by the weights in a 16-byte record {w0, w1, w2,
 * packed bone indices as a float} into 0x346010 + 64*i (rows 0..2, row 3 the
 * constant at 0x31A920), and hands the two halves to 0x1A2970. It was the
 * hottest game function in a match (~6.6% of the game thread; the same code
 * as Street 1's 0x1A03B0): the translation
 * keeps every temporary in volatile guest memory.
 *
 * A port of the translated code: the same single-precision lane operations in
 * the same order, the weight tests on the float bits as the original's
 * `test ecx, ecx`, and the index conversion truncating as the translation's
 * cvtss2si does, so the matrices are bit-identical.
 * RECOMP_GAME_NATIVE_SKIN=0 runs the translation; RECOMP_GAME_NATIVE_CHECK=1
 * runs both and compares every matrix (MISMATCH in the log); RECOMP_HLE_AB=2
 * alternates them every 600 frames.
 * ------------------------------------------------------------------------- */
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "hle_posix_win32.h"
#endif

void sub_001A4BF0_gen(void);
void sub_001A2970(void);
extern int hle_ab_new_off;

#define SKIN_OUT    0x346010u
#define SKIN_ROW3   0x31A920u

static int s_native_skin = -1, s_native_check;
static unsigned long long s_skin_calls, s_skin_mismatch;

static int native_skin_on(void)
{
    if (s_native_skin < 0) {
        const char *e = getenv("RECOMP_GAME_NATIVE_SKIN");
        s_native_skin = !(e && *e == '0');
        e = getenv("RECOMP_GAME_NATIVE_CHECK");
        s_native_check = e && *e && *e != '0';
        fprintf(stderr, "[GAME-NATIVE] skinning blend %s%s\n", s_native_skin ? "native" : "translated",
                s_native_check ? " (check mode)" : "");
    }
    return s_native_skin && !hle_ab_new_off;
}

/* Blend matrices [first, first + n) of the record list into out (host). */
static void skin_blend(uint32_t rec, uint32_t first, int32_t n, uint32_t pal, float (*out)[16])
{
    const float *row3 = (const float *)XBOX_PTR(SKIN_ROW3);
    int32_t i;
    for (i = 0; i < n; i++) {
        uint32_t r = rec + 16u * (first + (uint32_t)i), b1, b2, idx, m;
        float w0 = MEMF(r), w1 = MEMF(r + 4), w2 = MEMF(r + 8), acc[12];
        const float *src;
        int l;
        b1 = MEM32(r + 4);
        b2 = MEM32(r + 8);
        idx = (uint32_t)(int32_t)MEMF(r + 12);
        src = (const float *)XBOX_PTR(pal + ((idx >> 24) << 6));
        for (l = 0; l < 12; l++)
            acc[l] = src[l] * w0;
        if (b1 != 0) {
            m = pal + (((idx >> 16) & 0xFFu) << 6);
            src = (const float *)XBOX_PTR(m);
            for (l = 0; l < 12; l++) {
                float t = src[l] * w1;
                acc[l] = acc[l] + t;
            }
            if (b2 != 0) {
                m = pal + (((idx >> 8) & 0xFFu) << 6);
                src = (const float *)XBOX_PTR(m);
                for (l = 0; l < 12; l++) {
                    float t = src[l] * w2;
                    acc[l] = acc[l] + t;
                }
            }
        }
        memcpy(out[i], acc, 48);
        memcpy(&out[i][12], row3, 16);
    }
}

static void skin_hand_off(uint32_t dest, uint32_t mats, uint32_t bytes)
{
    PUSH32(g_esp, bytes);
    PUSH32(g_esp, mats);
    PUSH32(g_esp, dest);
    PUSH32(g_esp, 0u);
    sub_001A2970();
    g_esp += 12;                                   /* cdecl */
}

static float (*s_skin_buf)[16];
static int32_t s_skin_cap;

static void skin_native(void)
{
    uint32_t rec = MEM32(g_esp + 4), d1 = MEM32(g_esp + 12), d2 = MEM32(g_esp + 20), pal = MEM32(g_esp + 28);
    int32_t count = (int32_t)MEM32(g_esp + 8), by1 = (int32_t)MEM32(g_esp + 16), by2 = (int32_t)MEM32(g_esp + 24);
    int32_t n1 = by1 / 4, n2 = by2 / 4, total;
    if (n1 < count) {
        if (n1 + n2 >= count)
            n2 = count - n1;
    } else {
        n1 = count;
        n2 = 0;
    }
    total = (n1 > 0 ? n1 : 0) + (n2 > 0 ? n2 : 0);
    if (total > s_skin_cap) {
        s_skin_cap = total + 64;
        s_skin_buf = (float (*)[16])realloc(s_skin_buf, (size_t)s_skin_cap * sizeof *s_skin_buf);
    }
    if (n1 > 0) {
        skin_blend(rec, 0, n1, pal, s_skin_buf);
        memcpy((void *)XBOX_PTR(SKIN_OUT), s_skin_buf, (size_t)n1 * 64u);
    }
    skin_hand_off(d1, SKIN_OUT, (uint32_t)by1);
    if (n2 > 0) {
        uint32_t at = (uint32_t)n1;               /* as the original: the second half starts at n1 */
        skin_blend(rec, at, n2, pal, s_skin_buf);
        memcpy((void *)XBOX_PTR(SKIN_OUT + at * 64u), s_skin_buf, (size_t)n2 * 64u);
        skin_hand_off(d2, SKIN_OUT + at * 64u, (uint32_t)by2);
    }
    g_esp += 4;                                    /* ret */
}

static void skin_check(void)
{
    /* The blend is pure: compute it natively first, then let the translation
     * run (and hand off) and compare what it left in the matrix buffer. */
    uint32_t rec = MEM32(g_esp + 4), pal = MEM32(g_esp + 28);
    int32_t count = (int32_t)MEM32(g_esp + 8), by1 = (int32_t)MEM32(g_esp + 16), by2 = (int32_t)MEM32(g_esp + 24);
    int32_t n1 = by1 / 4, n2 = by2 / 4, total, i;
    static float (*mine)[16];
    static int32_t cap;
    if (n1 < count) {
        if (n1 + n2 >= count)
            n2 = count - n1;
    } else {
        n1 = count;
        n2 = 0;
    }
    total = (n1 > 0 ? n1 : 0) + (n2 > 0 ? n2 : 0);
    if (total > cap) {
        cap = total + 64;
        mine = (float (*)[16])realloc(mine, (size_t)cap * sizeof *mine);
    }
    if (total > 0)
        skin_blend(rec, 0, total, pal, mine);
    sub_001A4BF0_gen();
    for (i = 0; i < total; i++)
        if (memcmp(mine[i], (const void *)XBOX_PTR(SKIN_OUT + 64u * (uint32_t)i), 64)) {
            if (++s_skin_mismatch <= 16)
                fprintf(stderr, "[GAME-NATIVE] MISMATCH skin matrix %d of %d\n", i, total);
            break;
        }
    if ((++s_skin_calls & 4095u) == 0)
        fprintf(stderr, "[GAME-NATIVE] skin checked %llu calls, mismatches %llu\n", s_skin_calls, s_skin_mismatch);
}

/* RECOMP_HLE_PROFILE: time per call in each mode (printed every 8192 calls). */
static int s_skin_prof = -1;
static double s_skin_ms[2];
static unsigned long long s_skin_n[2];

static double skin_now_ms(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

void sub_001A4BF0(void)
{
    int smp = guest_cpu_native_enter(), native = native_skin_on();
    double t0 = 0.0;
    if (s_skin_prof < 0) {
        const char *e = getenv("RECOMP_HLE_PROFILE");
        s_skin_prof = e && *e && *e != '0';
    }
    if (s_skin_prof)
        t0 = skin_now_ms();
    if (!native)
        sub_001A4BF0_gen();
    else if (s_native_check)
        skin_check();
    else
        skin_native();
    if (s_skin_prof) {
        s_skin_ms[native] += skin_now_ms() - t0;
        if ((++s_skin_n[native] & 8191u) == 0)
            fprintf(stderr, "[GAME-NATIVE] skin blend: translated %.2f us/call (%llu), native %.2f us/call (%llu)\n",
                    s_skin_n[0] ? s_skin_ms[0] * 1000.0 / (double)s_skin_n[0] : 0.0, s_skin_n[0],
                    s_skin_n[1] ? s_skin_ms[1] * 1000.0 / (double)s_skin_n[1] : 0.0, s_skin_n[1]);
    }
    guest_cpu_native_leave(smp);
}

/* ---------------------------------------------------------------------------
 * Matrix multiply 0x25424B (stdcall out, a, b; out = a * b, rows of a times
 * b, all reads before the stores, eax = out) -- called from all over the
 * game -- and the bone matrix builder 0x1A7DD0 (thiscall skeleton, stdcall
 * bones, matrices, mask): with no mask, every bone's matrix from its scale,
 * quaternion and translation, times its parent's through the multiply hook
 * at 0x31AB60. Ports of the translated code, same operations in the same
 * order: bit-identical. Same switches as the skinning blend.
 * ------------------------------------------------------------------------- */
void sub_0025424B_gen(void);
void sub_001A7DD0_gen(void);

#define MATMUL_HOOK     0x31AB60u
#define MATMUL_WRAPPER  0x1AA680u    /* cdecl (out, b, a) -> 0x25424B(out, a, b) */
#define K_TWO           0x2AEB18u
#define K_ONE           0x2AE668u

static unsigned long long s_mm_calls, s_mm_mismatch, s_bone_calls, s_bone_mismatch;

static void mat_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    int i, l;
    for (i = 0; i < 4; i++)
        for (l = 0; l < 4; l++) {
            float x = a[4 * i] * b[l], y = a[4 * i + 1] * b[4 + l], z = a[4 * i + 2] * b[8 + l];
            float w = a[4 * i + 3] * b[12 + l], s = x + y;
            s = s + z;
            s = s + w;
            r[4 * i + l] = s;
        }
    memcpy(out, r, 64);
}

static void guest_mat_mul(uint32_t out, uint32_t a, uint32_t b)
{
    float fa[16], fb[16], fo[16];
    memcpy(fa, (const void *)XBOX_PTR(a), 64);
    memcpy(fb, (const void *)XBOX_PTR(b), 64);
    mat_mul(fo, fa, fb);
    memcpy((void *)XBOX_PTR(out), fo, 64);
}

void sub_0025424B(void)
{
    uint32_t out = MEM32(g_esp + 4), a = MEM32(g_esp + 8), b = MEM32(g_esp + 12);
    if (!native_skin_on()) {
        sub_0025424B_gen();
        return;
    }
    if (s_native_check) {
        float fa[16], fb[16], mine[16];
        memcpy(fa, (const void *)XBOX_PTR(a), 64);
        memcpy(fb, (const void *)XBOX_PTR(b), 64);
        mat_mul(mine, fa, fb);
        sub_0025424B_gen();
        if (memcmp(mine, (const void *)XBOX_PTR(out), 64) && ++s_mm_mismatch <= 16)
            fprintf(stderr, "[GAME-NATIVE] MISMATCH matrix multiply\n");
        if ((++s_mm_calls & 65535u) == 0)
            fprintf(stderr, "[GAME-NATIVE] matmul checked %llu calls, mismatches %llu\n", s_mm_calls, s_mm_mismatch);
        return;
    }
    guest_mat_mul(out, a, b);
    g_eax = out;
    g_esp += 16;                                   /* ret 12 */
}

/* One bone, translated loop of 0x1A7DD0: the local matrix into m. */
static void bone_local(uint32_t s, float m[16])
{
    float two = MEMF(K_TWO), one = MEMF(K_ONE);
    float s0 = MEMF(s), s1 = MEMF(s + 4), s2 = MEMF(s + 8), s3 = MEMF(s + 12);
    float qx = MEMF(s + 0x10), qy = MEMF(s + 0x14), qz = MEMF(s + 0x18), qw = MEMF(s + 0x1C);
    float x2 = qx * two, y2 = qy * two, z2 = qz * two;
    float wz = z2 * qw, xx = x2 * qx, wx = x2 * qw, xy = y2 * qx, wy = y2 * qw, xz = z2 * qx;
    float yy = y2 * qy, yz = z2 * qy, zz = z2 * qz, t;
    t = zz + yy; t = one - t; m[0] = t * s0;
    t = xy - wz; m[4] = t * s1;
    t = xy + wz; m[1] = t * s0;
    t = xz + wy; m[8] = t * s2;
    t = zz + xx; t = one - t; m[5] = t * s1;
    t = xz - wy; m[2] = t * s0;
    t = yz + wx; m[6] = t * s1;
    t = yz - wx; m[9] = t * s2;
    t = yy + xx; t = one - t; m[10] = t * s2;
    m[3] = m[7] = m[11] = 0.0f;
    m[12] = MEMF(s + 0x20); m[13] = MEMF(s + 0x24); m[14] = MEMF(s + 0x28); m[15] = one;
    m[0] = m[0] * s3;
    m[4] = s3 * m[4];
    m[8] = m[8] * s3;
}

static void bones_native(uint32_t self, uint32_t bones, uint32_t mats, int32_t count)
{
    int32_t i;
    for (i = 0; i < count; i++) {
        uint32_t d = mats + 0x40u * (uint32_t)i;
        int32_t parent = (int32_t)MEM32(self + 0x1C + 0x70u * (uint32_t)i);
        float m[16];
        bone_local(bones + 0x30u * (uint32_t)i, m);
        memcpy((void *)XBOX_PTR(d), m, 64);
        if (parent >= 0) {
            uint32_t p = mats + ((uint32_t)parent << 6);
            if (MEM32(MATMUL_HOOK) == MATMUL_WRAPPER) {
                guest_mat_mul(d, d, p);
            } else {
                PUSH32(g_esp, d); PUSH32(g_esp, p); PUSH32(g_esp, d); PUSH32(g_esp, 0u);
                recomp_lookup(MEM32(MATMUL_HOOK))();
                g_esp += 12;
            }
        }
    }
}

void sub_001A7DD0(void)
{
    uint32_t self = g_ecx, bones = MEM32(g_esp + 4), mats = MEM32(g_esp + 8), mask = MEM32(g_esp + 12);
    int32_t count = (int32_t)MEM32(self + 8);
    int smp;
    if (mask || !native_skin_on()) {
        sub_001A7DD0_gen();
        return;
    }
    smp = guest_cpu_native_enter();
    if (s_native_check && count > 0 && MEM32(MATMUL_HOOK) == MATMUL_WRAPPER) {
        /* Simulate on a copy (parents come from the same array), then let the
         * translation write the real one and compare. */
        static float (*sim)[16];
        static int32_t cap;
        int32_t i;
        if (count > cap) {
            cap = count + 16;
            sim = (float (*)[16])realloc(sim, (size_t)cap * 64u);
        }
        memcpy(sim, (const void *)XBOX_PTR(mats), (size_t)count * 64u);
        for (i = 0; i < count; i++) {
            int32_t parent = (int32_t)MEM32(self + 0x1C + 0x70u * (uint32_t)i);
            bone_local(bones + 0x30u * (uint32_t)i, sim[i]);
            if (parent >= 0) {
                float pm[16];
                if (parent < count) memcpy(pm, sim[parent], 64);
                else memcpy(pm, (const void *)XBOX_PTR(mats + ((uint32_t)parent << 6)), 64);
                mat_mul(sim[i], sim[i], pm);
            }
        }
        guest_cpu_native_leave(smp);
        sub_001A7DD0_gen();
        if (memcmp(sim, (const void *)XBOX_PTR(mats), (size_t)count * 64u) && ++s_bone_mismatch <= 16)
            fprintf(stderr, "[GAME-NATIVE] MISMATCH bone matrices (%d bones)\n", count);
        if ((++s_bone_calls & 4095u) == 0)
            fprintf(stderr, "[GAME-NATIVE] bones checked %llu calls, mismatches %llu\n", s_bone_calls, s_bone_mismatch);
        return;
    }
    bones_native(self, bones, mats, count);
    guest_cpu_native_leave(smp);
    g_esp += 16;                                   /* ret 12 */
}

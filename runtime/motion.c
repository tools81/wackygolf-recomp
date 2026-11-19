/* Motion smoothing: motion-compensated in-between pictures.

   The game shows new pictures 10-30 times a second (cartoon animation and
   moving objects streamed from the disc).  frame.c finds the next picture N
   by running the emulator ahead from the current one, C, so the in-betweens
   are true interpolations that add no delay.

   Motion C -> N is estimated per 8x8 block of a 384 x 240 luma image
   (normal-resolution pixels), coarse to fine and only where the picture
   changed.  Each 60 Hz frame between C and N is built from 4x4 blocks:
   among the vectors of nearby blocks, the one on which C (moved back) and
   N (moved forward) agree best is used, and the two are blended.  Blocks
   that no vector explains are cross-faded, and when more than a tenth of
   the change is not motion (drawn pose changes, zooms, cuts) the frame is
   not interpolated at all. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 768                   /* frame (texels) */
#define H 240
#define LW 384                  /* luma image (normal-resolution pixels) */
#define BS 8                    /* motion estimation block (luma pixels) */
#define BW (LW / BS)
#define BH (H / BS)
#define SB 4                    /* interpolation block */
#define SBW (LW / SB)
#define SBH (H / SB)

static uint32_t *picC, *picN;
static uint8_t *lumC, *lumN, *lumC2, *lumN2;   /* full and half resolution luma */
static int valid;               /* a usable pair is loaded */
static int16_t vx[BH][BW], vy[BH][BW];
static uint8_t changed[BH][BW];
static int16_t svx[SBH][SBW], svy[SBH][SBW];
static uint8_t smode[SBH][SBW];  /* 0 = C (unchanged/unexplained), 1 = interpolate, 2 = hard switch */
static float last_alpha = -1;

static void make_luma(const uint32_t *pic, uint8_t *l, uint8_t *l2) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < LW; x++) {
            uint32_t a = pic[y * W + 2 * x], b = pic[y * W + 2 * x + 1];
            int ya = (int)(((a >> 16) & 255) * 77 + ((a >> 8) & 255) * 150 + (a & 255) * 29) >> 8;
            int yb = (int)(((b >> 16) & 255) * 77 + ((b >> 8) & 255) * 150 + (b & 255) * 29) >> 8;
            l[y * LW + x] = (uint8_t)((ya + yb) >> 1);
        }
    for (int y = 0; y < H / 2; y++)
        for (int x = 0; x < LW / 2; x++) {
            const uint8_t *p = l + (2 * y) * LW + 2 * x;
            l2[y * (LW / 2) + x] = (uint8_t)((p[0] + p[1] + p[LW] + p[LW + 1] + 2) >> 2);
        }
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* SAD of an n x n block at (x, y) in a against (x + dx, y + dy) in b */
static int sad(const uint8_t *a, const uint8_t *b, int w, int h, int x, int y, int dx, int dy, int n, int best) {
    int s = 0;
    for (int j = 0; j < n; j++) {
        int yb = clampi(y + j + dy, 0, h - 1);
        const uint8_t *ra = a + (y + j) * w + x;
        const uint8_t *rb = b + yb * w;
        for (int i = 0; i < n; i++) {
            int d = ra[i] - rb[clampi(x + i + dx, 0, w - 1)];
            s += d < 0 ? -d : d;
        }
        if (s >= best) return s;
    }
    return s;
}

void motion_reset(void) { valid = 0; last_alpha = -1; }

/* load the pair C -> N and estimate motion; returns 1 if it is worth interpolating */
int motion_set_pair(const uint32_t *c, const uint32_t *n) {
    if (!picC) {
        picC = (uint32_t *)malloc(W * H * 4); picN = (uint32_t *)malloc(W * H * 4);
        lumC = (uint8_t *)malloc(LW * H); lumN = (uint8_t *)malloc(LW * H);
        lumC2 = (uint8_t *)malloc(LW * H / 4); lumN2 = (uint8_t *)malloc(LW * H / 4);
    }
    memcpy(picC, c, W * H * 4);
    memcpy(picN, n, W * H * 4);
    make_luma(picC, lumC, lumC2);
    make_luma(picN, lumN, lumN2);
    valid = 0;
    int nchanged = 0;
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) {
            int x = bx * BS, y = by * BS;
            int z = sad(lumC, lumN, LW, H, x, y, 0, 0, BS, 1 << 30);
            vx[by][bx] = vy[by][bx] = 0;
            changed[by][bx] = z > BS * BS * 3;
            if (!changed[by][bx]) continue;
            nchanged++;
            /* coarse: half resolution, +-12 x +-8 (= +-24 x +-16 full) */
            int best = 1 << 30, bdx = 0, bdy = 0;
            for (int dy = -8; dy <= 8; dy++)
                for (int dx = -12; dx <= 12; dx++) {
                    int sv = sad(lumC2, lumN2, LW / 2, H / 2, x / 2, y / 2, dx, dy, BS / 2, best);
                    if (sv < best || (sv == best && abs(dx) + abs(dy) < abs(bdx) + abs(bdy))) { best = sv; bdx = dx; bdy = dy; }
                }
            /* fine: full resolution, +-2 around the coarse vector */
            best = 1 << 30;
            int fx = 0, fy = 0;
            for (int dy = 2 * bdy - 2; dy <= 2 * bdy + 2; dy++)
                for (int dx = 2 * bdx - 2; dx <= 2 * bdx + 2; dx++) {
                    int sv = sad(lumC, lumN, LW, H, x, y, dx, dy, BS, best);
                    if (sv < best) { best = sv; fx = dx; fy = dy; }
                }
            vx[by][bx] = (int16_t)fx;
            vy[by][bx] = (int16_t)fy;
        }
    if (!nchanged) return 0;
    valid = 1;
    last_alpha = -1;
    return 1;
}

/* choose the vector of each interpolation block for phase a (0 = C, 1 = N) */
static int choose(float a) {
    int nchg = 0, ngood = 0;
    for (int sy = 0; sy < SBH; sy++)
        for (int sx = 0; sx < SBW; sx++) {
            int x = sx * SB, y = sy * SB;
            int by = y / BS, bx = x / BS;
            /* unchanged area: C */
            int any = 0;
            for (int j = -1; j <= 1 && !any; j++)
                for (int i = -1; i <= 1; i++) {
                    int yy = by + j, xx = bx + i;
                    if (yy >= 0 && yy < BH && xx >= 0 && xx < BW && changed[yy][xx]) { any = 1; break; }
                }
            smode[sy][sx] = 0;
            if (!any) continue;
            /* bilateral match: C at -a*v, N at +(1-a)*v */
            int best = 1 << 30, bvx = 0, bvy = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    int yy = clampi(by + j, 0, BH - 1), xx = clampi(bx + i, 0, BW - 1);
                    int cx = vx[yy][xx], cy = vy[yy][xx];
                    int ox = (int)(-a * cx + (cx < 0 ? -0.5f : 0.5f)), oy = (int)(-a * cy + (cy < 0 ? -0.5f : 0.5f));
                    int s = 0;
                    for (int q = 0; q < SB; q++) {
                        int yc = clampi(y + q + oy, 0, H - 1), yn = clampi(y + q + oy + cy, 0, H - 1);
                        for (int p = 0; p < SB; p++) {
                            int xc = clampi(x + p + ox, 0, LW - 1), xn = clampi(x + p + ox + cx, 0, LW - 1);
                            int d = lumC[yc * LW + xc] - lumN[yn * LW + xn];
                            s += d < 0 ? -d : d;
                        }
                    }
                    if (s < best) { best = s; bvx = cx; bvy = cy; }
                }
            /* did C and N disagree here at all? */
            int z = 0;
            for (int q = 0; q < SB; q++)
                for (int p = 0; p < SB; p++) {
                    int d = lumC[(y + q) * LW + x + p] - lumN[(y + q) * LW + x + p];
                    z += d < 0 ? -d : d;
                }
            if (z <= SB * SB * 2) continue;           /* this small block did not change */
            nchg++;
            svx[sy][sx] = (int16_t)bvx;
            svy[sy][sx] = (int16_t)bvy;
            if (best <= SB * SB * 4 && (bvx || bvy)) { smode[sy][sx] = 1; ngood++; }
            else smode[sy][sx] = 2;
        }
    /* any sizeable unexplained change (drawn animation, zooms): leave the
       frame alone; a patchwork of moved and unmoved blocks looks worse */
    return nchg && ngood * 10 >= nchg * 9;
}

static inline uint32_t blend(uint32_t c, uint32_t n, int wn) {   /* wn: 0-256 */
    int wc = 256 - wn;
    uint32_t r = ((((c >> 16) & 255) * wc + ((n >> 16) & 255) * wn) >> 8);
    uint32_t g = ((((c >> 8) & 255) * wc + ((n >> 8) & 255) * wn) >> 8);
    uint32_t b = (((c & 255) * wc + (n & 255) * wn) >> 8);
    return (r << 16) | (g << 8) | b;
}

/* render the in-between at phase a into out (which holds the real picture);
   returns 1 if the picture was changed */
int motion_render(uint32_t *out, float a) {
    if (!valid || a <= 0.0f || a >= 1.0f) return 0;
    if (a != last_alpha) {
        last_alpha = a;
        if (!choose(a)) { valid = 0; return 0; }
    }
    int wn = (int)(a * 256 + 0.5f);
    for (int sy = 0; sy < SBH; sy++)
        for (int sx = 0; sx < SBW; sx++) {
            int m = smode[sy][sx];
            if (!m) continue;
            int x0 = sx * SB * 2, y0 = sy * SB;
            if (m == 2) {                               /* the few unexplained blocks: cross-fade */
                for (int q = 0; q < SB; q++)
                    for (int p = 0; p < SB * 2; p++) {
                        int o = (y0 + q) * W + x0 + p;
                        out[o] = blend(picC[o], picN[o], wn);
                    }
                continue;
            }
            int cx = svx[sy][sx], cy = svy[sy][sx];
            int ox = (int)(-a * cx * 2 + (cx < 0 ? -0.5f : 0.5f)), oy = (int)(-a * cy + (cy < 0 ? -0.5f : 0.5f));
            for (int q = 0; q < SB; q++) {
                int yc = clampi(y0 + q + oy, 0, H - 1), yn = clampi(y0 + q + oy + cy, 0, H - 1);
                for (int p = 0; p < SB * 2; p++) {
                    int xc = clampi(x0 + p + ox, 0, W - 1), xn = clampi(x0 + p + ox + cx * 2, 0, W - 1);
                    out[(y0 + q) * W + x0 + p] = blend(picC[yc * W + xc], picN[yn * W + xn], wn);
                }
            }
        }
    return 1;
}

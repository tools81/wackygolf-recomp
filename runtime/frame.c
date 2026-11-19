/* Frame loop: presentation, run-ahead and quicksave slots.

   The run loop calls frame_boundary() at a safe point (no 68000 code in
   progress on the native stack) after the video module has rendered a field.

   Run-ahead (N frames): after each real field the machine state is saved,
   N more fields are emulated with the same input ("speculative": no audio,
   no logging, no disk writes), the last of them is shown, and the state is
   restored.  What is on screen is N fields ahead of the real machine, so the
   reaction to new input appears N fields sooner.  The real timeline (audio,
   saves, NVRAM) is unaffected.

   Quicksave: 10 slots, kept in memory and in saves/quicksave_<n>.state. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int g_speculative;
volatile int g_field_ready;

/* settings (host_win32.c loads and saves them) */
int g_runahead = 1;          /* fields shown ahead of the real machine (0-3) */
int g_cursor_smooth = 1;     /* draw the cursor at the newest pointer position every field */
int g_motion_smooth = 0;     /* motion-compensated in-between pictures (motion.c), F7 */
int g_pacing = 1;            /* even out the intervals between the game's pictures */

/* command-line overrides (-1 = not given) */
int g_runahead_cli = -1, g_motion_cli = -1, g_cursor_smooth_cli = -1, g_pacing_cli = -1;

/* after the host has read settings.ini: the command line wins; headless
   (test) runs default to no run-ahead so their frames are the real ones */
void frame_apply_options(void) {
    extern int g_headless;
    if (g_headless) { g_runahead = 0; g_motion_smooth = 0; g_pacing = 0; }
    if (g_pacing_cli >= 0) g_pacing = g_pacing_cli;
    if (g_runahead_cli >= 0) g_runahead = g_runahead_cli > 3 ? 3 : g_runahead_cli;
    if (g_motion_cli >= 0) g_motion_smooth = g_motion_cli;
    if (g_cursor_smooth_cli >= 0) g_cursor_smooth = g_cursor_smooth_cli;
}

/* requests from the host (hotkeys, scripted input) */
volatile int g_req_save, g_req_load, g_req_slot, g_req_runahead, g_req_motion, g_req_pacing;

#define NSLOTS 10
static StateIO slots[NSLOTS];
static int slot;              /* 0-based */
static StateIO ra;            /* run-ahead snapshot */
static int spec_left;
static uint32_t *out_fb;      /* picture being presented */

void video_picture(uint32_t *out);
void video_draw_cursor(uint32_t *out, int smooth);
void motion_process(uint32_t *pic);
void motion_reset(void);
void nvr_flush(void);
void nvr_mark_all_dirty(void);
int  drvext_system_state(void);
int  os9_child_running(void);
void host_present_frame(const uint32_t *fb, int w, int h);
void host_settings_changed(void);

/* ---------------------------------------------------------------- on-screen messages */
static char osd_text[64];
static int osd_frames;

static void osd(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_text, sizeof osd_text, fmt, ap);
    va_end(ap);
    osd_frames = 120;
    LOG("%s", osd_text);
}

/* 5x7 font: one row per byte, bit 4 = leftmost column */
static const struct { char c; uint8_t r[7]; } font[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++) if (font[i].c == c) return font[i].r;
    return NULL;
}

/* text in the top-left corner; a font pixel is 3 x 1 frame pixels (about square on screen) */
static void osd_draw(uint32_t *fb, int w, int h, int tick) {
    if (osd_frames <= 0) return;
    if (tick) osd_frames--;
    const int sx = 3, cw = 6 * sx, x0 = 12, y0 = 6;
    int len = (int)strlen(osd_text);
    int bw = len * cw + 2 * sx * 2, bh = 7 + 4;
    for (int y = y0 - 2; y < y0 - 2 + bh && y < h; y++)
        for (int x = x0 - 2 * sx; x < x0 - 2 * sx + bw && x < w; x++) {
            uint32_t p = fb[y * w + x];
            fb[y * w + x] = (p >> 2) & 0x3F3F3F;           /* darkened box */
        }
    for (int i = 0; i < len; i++) {
        const uint8_t *g = glyph(osd_text[i]);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int k = 0; k < sx; k++) {
                    int x = x0 + i * cw + c * sx + k, y = y0 + r;
                    if (x < w && y < h) fb[y * w + x] = 0xFFFFE070;
                }
            }
    }
}

/* phases of the frame loop */
enum { PH_REAL, PH_AHEAD, PH_LOOK };
static int phase;
static int look_n;                /* fields run ahead while looking for the next picture */
static long long look_gap_prev;   /* interval before the picture being looked ahead from */

/* will the field being rendered now be shown or examined?  (run-ahead shows
   only the last field of its window; look-ahead examines every field) */
int frame_field_shown(void) {
    if (phase == PH_LOOK) return 1;
    if (phase == PH_AHEAD) return spec_left <= 1;
    return g_runahead == 0 || run_depth() != 1;
}

/* Pictures, pacing and motion smoothing.

   The game shows a new picture every few fields (10-30 a second).  The disc
   delivers a picture every 7.5 sectors (alternately 7 and 8) and the game's
   decoding time varies, so a 10 fps animation lands on fields 5 and 7 apart
   instead of 6 and 6: visible judder.  When a new picture appears, the frame
   loop runs ahead (look-ahead) to find when the next one will appear.  If
   this one came early compared with the next interval, the previous picture
   stays up for a field or two longer (pacing), so the intervals even out.
   The same look-ahead gives motion smoothing its next picture (motion.c). */
int  motion_set_pair(const uint32_t *c, const uint32_t *n);
int  motion_render(uint32_t *out, float a);
static uint32_t *picC, *picP;     /* the newest game picture, and the one before it */
static uint32_t fpC;              /* fingerprint of picC */
static long long disp;            /* displayed frames */
static long long tCr, tCe, tN;    /* when C appeared, when it is shown (paced), when N appears */
static int n_valid;               /* motion pair C -> N loaded */
static int have_p;
static int c_logged;
static uint32_t *outP;            /* output with the previous picture (pacing) */

static int tracking(void) { return g_motion_smooth || g_pacing; }

/* recent intervals between the game's pictures (fields), for pacing without
   look-ahead */
static int hist[4], nhist;
static int paced_hold(long long g1) {
    int d = 0;
    if (nhist >= 3 && g1 >= 3 && g1 <= 9) {
        int sum = 0;
        for (int i = 0; i < nhist; i++) sum += hist[i];
        double avg = (double)sum / nhist;
        if (g1 <= avg - 0.75) d = (int)(avg - g1 + 0.5);
        if (d > 2) d = 2;
    }
    if (g1 >= 3 && g1 <= 9) {
        if (nhist < 4) hist[nhist++] = (int)g1;
        else { memmove(hist, hist + 1, 3 * sizeof hist[0]); hist[3] = (int)g1; }
    } else nhist = 0;               /* a pause or a cut: start over */
    return d;
}

static uint32_t fingerprint(const uint32_t *p) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < video_w * video_h; i++) h = (h ^ p[i]) * 16777619u;
    return h | 1;
}

static int cadence_log = -1;      /* RECOMP_CADENCE=1: log the fields where the shown picture changes */
static void log_shown(void) {
    if (cadence_log < 0) cadence_log = getenv("RECOMP_CADENCE") != NULL;
    if (cadence_log) logf_(0, "PIC %lld", disp);
}

/* the output picture into out: game picture (possibly an in-between), cursor, messages */
static void build_into(uint32_t *out, const uint32_t *pic, int motion, int tick_osd) {
    memcpy(out, pic, (size_t)video_w * video_h * 4);
    if (motion && g_motion_smooth && n_valid && tN > tCe && disp >= tCe)
        motion_render(out, (float)(disp - tCe) / (float)(tN - tCe));
    video_draw_cursor(out, g_cursor_smooth);
    osd_draw(out, video_w, video_h, tick_osd);
}

static void build_output(const uint32_t *pic, int motion) {
    if (!out_fb) out_fb = (uint32_t *)malloc((size_t)video_w * video_h * 4);
    build_into(out_fb, pic, motion, 1);
}

/* RECOMP_FRAMETIME=1: distribution of the work per displayed frame, logged at exit */
static double ft_last;
static int ft_hist[8];      /* <4, <8, <12, <16, <20, <25, <33, >=33 ms */
static double ft_max;
static void ft_report(void) {
    logf_(0, "frame work ms: <4 %d, <8 %d, <12 %d, <16 %d, <20 %d, <25 %d, <33 %d, >=33 %d; max %.1f",
          ft_hist[0], ft_hist[1], ft_hist[2], ft_hist[3], ft_hist[4], ft_hist[5], ft_hist[6], ft_hist[7], ft_max);
}

static void show(uint32_t *fb) {
    static int on = -1;
    if (on < 0) { on = getenv("RECOMP_FRAMETIME") != NULL; if (on) atexit(ft_report); }
    if (on) {
        double now = host_time();
        if (ft_last > 0) {
            double ms = (now - ft_last) * 1000.0;
            static const double lim[7] = {4, 8, 12, 16, 20, 25, 33};
            int k = 0;
            while (k < 7 && ms >= lim[k]) k++;
            ft_hist[k]++;
            if (ms > ft_max) ft_max = ms;
        }
    }
    host_present_frame(fb, video_w, video_h);
    host_poll();
    if (on) ft_last = host_time();
}

/* back to the real machine after run-ahead or look-ahead */
static void end_speculation(void) {
    if (phase == PH_REAL) return;
    phase = PH_REAL;
    g_speculative = 0;
    if (state_load(&ra)) fatal("run-ahead snapshot could not be restored");
}

/* the field to show has been rendered (real, or the end of a run-ahead window) */
static void display(void) {
    static uint32_t *cur;
    if (!cur) cur = (uint32_t *)malloc((size_t)video_w * video_h * 4);
    disp++;
    video_picture(cur);
    if (!tracking()) {
        uint32_t fp = fingerprint(cur);
        if (fp != fpC) { fpC = fp; log_shown(); }
        build_output(cur, 0);
        show(out_fb);
        end_speculation();
        return;
    }
    if (!picC) {
        picC = (uint32_t *)malloc((size_t)video_w * video_h * 4);
        picP = (uint32_t *)malloc((size_t)video_w * video_h * 4);
        outP = (uint32_t *)malloc((size_t)video_w * video_h * 4);
    }
    uint32_t fp = fingerprint(cur);
    if (fp == fpC) {                      /* no new picture: paced hold or an in-between */
        if (disp < tCe) build_output(picP, 0);
        else {
            if (!c_logged) { c_logged = 1; log_shown(); }
            build_output(picC, 1);
        }
        show(out_fb);
        end_speculation();
        return;
    }
    /* a new picture: remember the previous one, then look ahead for the next */
    long long gap_prev = have_p ? disp - tCr : 0;
    if (have_p || fpC) { memcpy(picP, picC, (size_t)video_w * video_h * 4); have_p = 1; }
    memcpy(picC, cur, (size_t)video_w * video_h * 4);
    fpC = fp;
    tCr = tCe = disp;
    n_valid = 0;
    c_logged = 0;
    tN = 0;
    look_gap_prev = gap_prev;
    /* both possible outputs, with the cursor as it is now */
    build_into(outP, picP, 0, 0);
    build_output(picC, 0);
    if (!g_motion_smooth) {
        /* pacing from the recent intervals: no look-ahead, no cost */
        if (g_pacing && have_p) tCe = tCr + paced_hold(gap_prev);
        if (disp < tCe) show(outP);
        else { c_logged = 1; log_shown(); show(out_fb); }
        end_speculation();
        return;
    }
    if (phase == PH_REAL) state_save(&ra, 0);
    phase = PH_LOOK;
    g_speculative = 1;
    look_n = 0;
}

static void finish_look(int found) {
    if (found) {
        tN = tCr + look_n;
        if (g_motion_smooth) {
            static uint32_t *pn;
            if (!pn) pn = (uint32_t *)malloc((size_t)video_w * video_h * 4);
            video_picture(pn);
            n_valid = motion_set_pair(picC, pn);
        }
        /* came early: hold the previous picture to even out the intervals */
        long long g1 = look_gap_prev, g2 = look_n;
        if (g_pacing && have_p && g1 >= 3 && g1 <= 9 && g2 <= 9 && g2 - g1 >= 2)
            tCe = tCr + (g2 - g1) / 2;
        paced_hold(g1);                   /* keep the interval history current */
    }
    if (disp < tCe) show(outP);
    else { c_logged = 1; log_shown(); show(out_fb); }
    end_speculation();
}

/* ---------------------------------------------------------------- quicksave */
static void slot_path(int n, char *out, size_t len) {
    snprintf(out, len, "%s/quicksave_%d.state", g_data_dir, n + 1);
}

static void quicksave(void) {
    state_save(&slots[slot], 1);
    char p[600];
    slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(slots[slot].buf, 1, slots[slot].len, f); fclose(f); }
    osd("Saved slot %d", slot + 1);
}

static void quickload(void) {
    StateIO *s = &slots[slot];
    if (!s->len) {
        char p[600];
        slot_path(slot, p, sizeof p);
        FILE *f = fopen(p, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                s->buf = (uint8_t *)realloc(s->buf, (size_t)n);
                s->cap = (size_t)n;
                s->len = fread(s->buf, 1, (size_t)n, f);
            }
            fclose(f);
        }
    }
    if (!s->len) { osd("Slot %d is empty", slot + 1); return; }
    /* keep the current state in case the slot does not fit this build */
    StateIO backup = {0};
    state_save(&backup, 1);
    if (state_load(s)) {
        state_load(&backup);
        osd("Slot %d: saved by another version", slot + 1);
    } else {
        nvr_mark_all_dirty();
        motion_reset();
        fpC = 0;
        osd("Loaded slot %d", slot + 1);
    }
    state_free(&backup);
}

static void handle_requests(void) {
    if (g_req_slot) {
        g_req_slot = 0;
        slot = (slot + 1) % NSLOTS;
        char p[600];
        slot_path(slot, p, sizeof p);
        FILE *f = fopen(p, "rb");
        int used = slots[slot].len || f;
        if (f) fclose(f);
        osd("Slot %d%s", slot + 1, used ? "" : " (empty)");
    }
    if (g_req_save) { g_req_save = 0; quicksave(); }
    if (g_req_load) { g_req_load = 0; quickload(); }
    if (g_req_runahead) {
        g_req_runahead = 0;
        g_runahead = (g_runahead + 1) % 4;
        osd("Run-ahead %d", g_runahead);
        host_settings_changed();
    }
    if (g_req_pacing) {
        g_req_pacing = 0;
        g_pacing = !g_pacing;
        fpC = 0;
        osd("Frame pacing %s", g_pacing ? "on" : "off");
        host_settings_changed();
    }
    if (g_req_motion) {
        g_req_motion = 0;
        g_motion_smooth = !g_motion_smooth;
        motion_reset();
        fpC = 0;
        n_valid = 0;
        osd("Motion smoothing %s", g_motion_smooth ? "on" : "off");
        host_settings_changed();
    }
}

/* ---------------------------------------------------------------- frame boundary */
#define LOOK_MAX 8                        /* fields to look ahead for the next picture */

void frame_boundary(void) {
    int fields = g_field_ready;
    /* snapshots need the outermost run loop, outside driver code and child processes */
    if (run_depth() != 1 || drvext_system_state() || os9_child_running()) {
        if (phase != PH_REAL) return;     /* finish the speculative window first */
        g_field_ready = 0;
        static uint32_t *tmp;
        if (!tmp) tmp = (uint32_t *)malloc((size_t)video_w * video_h * 4);
        video_picture(tmp);               /* keep the display alive, nothing else */
        build_output(tmp, 0);
        show(out_fb);
        return;
    }
    g_field_ready = 0;
    if (phase == PH_AHEAD) {
        spec_left -= fields;
        if (spec_left > 0) return;
        display();                        /* the picture from the future */
        return;
    }
    if (phase == PH_LOOK) {
        look_n += fields;
        static uint32_t *tmp;
        if (!tmp) tmp = (uint32_t *)malloc((size_t)video_w * video_h * 4);
        video_picture(tmp);
        int changed = fingerprint(tmp) != fpC;
        if (changed || look_n >= LOOK_MAX) finish_look(changed);
        return;
    }
    /* a real field: side effects of the real timeline happen here */
    nvr_flush();
    handle_requests();
    if (g_runahead > 0) {
        state_save(&ra, 0);
        phase = PH_AHEAD;
        g_speculative = 1;
        spec_left = g_runahead;
        return;
    }
    display();
}

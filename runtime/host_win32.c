/* Win32 host platform: window, input, audio, timing. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "render.h"
#include "game_config.h"

static int fullscreen;
static void save_settings(void);
static void update_vsync(void);
extern int g_runahead, g_cursor_smooth, g_motion_smooth, g_pacing;
extern volatile int g_req_save, g_req_load, g_req_slot, g_req_runahead, g_req_motion, g_req_pacing;
extern int g_speculative;

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static HWND hwnd;
static LARGE_INTEGER qpf, qp0;
static int keys[256];
int g_headless;
static int win_scale = 3;

static WINDOWPLACEMENT saved_wp = {sizeof(WINDOWPLACEMENT)};

static void toggle_fullscreen(void) {
    DWORD style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(hwnd, &saved_wp) &&
            GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            ShowCursor(FALSE);
        }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &saved_wp);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(TRUE);
    }
    fullscreen = !fullscreen;
    update_vsync();
    save_settings();
}

/* ---------------------------------------------------------------- video settings */
static int use_gpu = 1, start_fullscreen, win_w, win_h;
int g_vsync = 1;                    /* sync to the display when its refresh is a multiple of 60 Hz */
static int vsync_n;                 /* refreshes per field (0: timer pacing) */
static int refresh_hz;
static int gpu_ok;

static void settings_path(char *out, size_t n) { snprintf(out, n, "%s/settings.ini", g_data_dir); }

static void load_settings(void) {
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "aspect=%d", &v) == 1) render_settings.aspect = v % RENDER_ASPECTS;
        if (sscanf(line, "filter=%d", &v) == 1) render_settings.filter = v % RENDER_FILTERS;
        if (sscanf(line, "fullscreen=%d", &v) == 1) start_fullscreen = v;
        if (sscanf(line, "runahead=%d", &v) == 1) g_runahead = v < 0 ? 0 : v > 3 ? 3 : v;
        if (sscanf(line, "cursor_smooth=%d", &v) == 1) g_cursor_smooth = v != 0;
        if (sscanf(line, "motion_smooth=%d", &v) == 1) g_motion_smooth = v != 0;
        if (sscanf(line, "pacing=%d", &v) == 1) g_pacing = v != 0;
        if (sscanf(line, "vsync=%d", &v) == 1) g_vsync = v != 0;
    }
    fclose(f);
}

static void save_settings(void) {
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "aspect=%d\nfilter=%d\nfullscreen=%d\nrunahead=%d\ncursor_smooth=%d\nmotion_smooth=%d\npacing=%d\nvsync=%d\n", render_settings.aspect,
            render_settings.filter, fullscreen, g_runahead, g_cursor_smooth, g_motion_smooth, g_pacing, g_vsync);
    fclose(f);
}

static void update_title(void) {
    char t[256];
    if (gpu_ok)
        snprintf(t, sizeof t, GAME_TITLE "  [%s, %s, run-ahead %d%s]  F2 save, F3 slot, F4 load, F6 run-ahead, F7 motion, F8 pacing, F9 aspect, F10 filter, F11 fullscreen",
                 render_aspect_name(render_settings.aspect), render_filter_name(render_settings.filter), g_runahead,
                 g_motion_smooth ? ", motion smoothing" : "");
    if (gpu_ok) {
        size_t L = strlen(t);
        if (vsync_n) snprintf(t + L, sizeof t - L, "  (vsync %d Hz)", refresh_hz);
    }
    else
        snprintf(t, sizeof t, GAME_TITLE "  (GDI)");
    SetWindowTextA(hwnd, t);
}

static void settings_changed(void) {
    update_title();
    save_settings();
}

/* command-line overrides: --aspect N, --filter N, --fullscreen, --windowed, --gdi, --size WxH */
void host_video_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--aspect")) render_settings.aspect = atoi(val) % RENDER_ASPECTS;
    else if (!strcmp(opt, "--filter")) render_settings.filter = atoi(val) % RENDER_FILTERS;
    else if (!strcmp(opt, "--fullscreen")) start_fullscreen = 1;
    else if (!strcmp(opt, "--windowed")) start_fullscreen = 0;
    else if (!strcmp(opt, "--gdi")) use_gpu = 0;
    else if (!strcmp(opt, "--size") && val) sscanf(val, "%dx%d", &win_w, &win_h);
}

/* where the picture is drawn in a cw x ch client area (same layout as the
   presenters: Direct3D follows the aspect mode, GDI letterboxes to 4:3) */
static void picture_rect(int cw, int ch, double *px, double *py, double *pw, double *ph) {
    double aspect = 4.0 / 3.0, w, hh;
    int mode = gpu_ok ? render_settings.aspect % RENDER_ASPECTS : 1;
    if (mode == 2) { w = cw; hh = ch; }
    else if (mode == 3) { w = cw; hh = cw / aspect; if (hh < ch) { hh = ch; w = ch * aspect; } }
    else if ((double)cw / ch > aspect) { hh = ch; w = ch * aspect; }
    else { w = cw; hh = cw / aspect; }
    *pw = w; *ph = hh; *px = (cw - w) / 2; *py = (ch - hh) / 2;
}

/* mouse: absolute pointer in CD-i coordinates (768 x 480) */
static int mouse_x = -1, mouse_y = -1, mouse_moved, mouse_b1, mouse_b2;

static void mouse_track(HWND h, LPARAM l) {
    RECT r;
    GetClientRect(h, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    double px, py, pw, ph;
    picture_rect(r.right, r.bottom, &px, &py, &pw, &ph);
    int mx = (short)LOWORD(l), my = (short)HIWORD(l);
    int x = (int)((mx - px) * 768.0 / pw), y = (int)((my - py) * 480.0 / ph);
    if (x < 0) x = 0;
    if (x > 767) x = 767;
    if (y < 0) y = 0;
    if (y > 479) y = 479;
    if (x != mouse_x || y != mouse_y) { mouse_x = x; mouse_y = y; mouse_moved = 1; }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: input.quit = 1; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        keys[w & 255] = 1;
        if (w == VK_ESCAPE) input.quit = 1;
        if (!(l & 0x40000000)) {              /* not auto-repeat */
            if (w == VK_F2) g_req_save = 1;
            if (w == VK_F3) g_req_slot = 1;
            if (w == VK_F4) g_req_load = 1;
            if (w == VK_F6) g_req_runahead = 1;
            if (w == VK_F7) g_req_motion = 1;
            if (w == VK_F8) g_req_pacing = 1;
        }
        if (w == VK_F9) { render_settings.aspect = (render_settings.aspect + 1) % RENDER_ASPECTS; settings_changed(); }
        if (w == VK_F10) { render_settings.filter = (render_settings.filter + 1) % RENDER_FILTERS; settings_changed(); }
        if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
            toggle_fullscreen();
            keys[VK_RETURN] = 0;
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SYSCHAR:
        return 0;   /* no menu beep on Alt+Enter */
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYUP: case WM_SYSKEYUP:
        keys[w & 255] = 0;
        return 0;
    case WM_KILLFOCUS:
        memset(keys, 0, sizeof keys);
        mouse_b1 = mouse_b2 = 0;
        return 0;
    case WM_MOUSEMOVE:
        mouse_track(h, l);
        return 0;
    case WM_LBUTTONDOWN: mouse_track(h, l); mouse_b1 = 1; SetCapture(h); return 0;
    case WM_LBUTTONUP:   mouse_b1 = 0; if (!mouse_b2) ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN: mouse_track(h, l); mouse_b2 = 1; SetCapture(h); return 0;
    case WM_RBUTTONUP:   mouse_b2 = 0; if (!mouse_b1) ReleaseCapture(); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int host_init(void) {
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qp0);
    timeBeginPeriod(1);
    if (g_headless) return 0;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CDiRecomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    SetProcessDPIAware();   /* render at true pixel resolution on high-DPI / 4K displays */
    int save_aspect = render_settings.aspect, save_filter = render_settings.filter;
    int cli_fs = start_fullscreen;
    load_settings();
    /* command-line options win over the settings file */
    if (save_aspect) render_settings.aspect = save_aspect;
    if (save_filter) render_settings.filter = save_filter;
    if (cli_fs) start_fullscreen = 1;
    int cw = win_w > 0 ? win_w : 320 * win_scale, chh = win_h > 0 ? win_h : 240 * win_scale;
    if (win_w <= 0) {
        /* default: a 16:9 window filling most of the screen height */
        int sh = GetSystemMetrics(SM_CYSCREEN);
        chh = sh * 3 / 4;
        cw = chh * 16 / 9;
    }
    RECT r = {0, 0, cw, chh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, GAME_TITLE,
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return -1;
    if (use_gpu) gpu_ok = render_init(hwnd) == 0;
    update_vsync();
    if (start_fullscreen) toggle_fullscreen();
    update_title();
    return 0;
}

double host_time(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qp0.QuadPart) / (double)qpf.QuadPart;
}

void host_sleep_until(double t) {
    for (;;) {
        double now = host_time();
        if (now >= t) return;
        double d = t - now;
        if (d > 0.002) Sleep((DWORD)((d - 0.001) * 1000));
    }
}

int g_turbo;
int g_fuzz;
static double next_frame_time = -1;
static unsigned long long frame_counter;
int g_dump_frames;      /* write frames to BMP files (debug) */
int g_max_frames;       /* exit after N fields (debug) */
int g_dump_from;        /* first field to dump (debug) */
long long g_capture_frame;          /* debug: offscreen filter captures */
int g_capture_w = 1920, g_capture_h = 1440;
char g_capture_filters[16] = "045", g_capture_prefix[512] = "capture";

static void dump_bmp(const uint32_t *fb, int w, int h, unsigned long long n) {
    char name[256];
    snprintf(name, sizeof name, "%s/frame_%06llu.bmp", g_data_dir, n);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    BITMAPFILEHEADER bf = {0};
    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
    bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + w * h * 4;
    fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f);
    fwrite(fb, 4, (size_t)w * h, f);
    fclose(f);
}

static HWAVEOUT wo;
#define NBUF 12
int host_audio_queued(void);

/* sync to the display when its refresh rate is (close to) a multiple of 60 Hz */
static void update_vsync(void) {
    vsync_n = 0;
    refresh_hz = 0;
    if (!gpu_ok || !hwnd) return;
    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        refresh_hz = (int)dm.dmDisplayFrequency;
    if (g_vsync)
        for (int n = 1; n <= 4; n++)
            if (refresh_hz >= 60 * n - 1 && refresh_hz <= 60 * n + 1) vsync_n = n;
}

void host_settings_changed(void) { if (hwnd) settings_changed(); else save_settings(); }

static int pi_hist[6]; static double pi_last, pi_max; static int pi_on = -1;
static void pi_report(void) {
    logf_(0, "present intervals ms: <15 %d, 15-18 %d, 18-25 %d, 25-35 %d, 35-50 %d, >=50 %d; max %.1f; refresh %d Hz, vsync %d",
          pi_hist[0], pi_hist[1], pi_hist[2], pi_hist[3], pi_hist[4], pi_hist[5], pi_max, refresh_hz, vsync_n);
}
static void pi_note(void) {         /* RECOMP_PRESENTLOG=1: intervals between presented frames */
    if (pi_on < 0) { pi_on = getenv("RECOMP_PRESENTLOG") != NULL; if (pi_on) atexit(pi_report); }
    if (!pi_on) return;
    double t = host_time();
    if (pi_last > 0 && frame_counter > 120) {
        double ms = (t - pi_last) * 1000;
        int k = ms < 15 ? 0 : ms < 18 ? 1 : ms < 25 ? 2 : ms < 35 ? 3 : ms < 50 ? 4 : 5;
        pi_hist[k]++;
        if (ms > pi_max) pi_max = ms;
    }
    pi_last = t;
}

void host_present_frame(const uint32_t *fb, int w, int h) {
    frame_counter++;
    if (g_dump_frames && frame_counter >= (unsigned long long)g_dump_from && frame_counter % (unsigned)g_dump_frames == 0)
        dump_bmp(fb, w, h, frame_counter);
    if (g_max_frames && frame_counter >= (unsigned long long)g_max_frames) {
        logf_(0, "reached %d fields, exiting", g_max_frames);   /* also from a run-ahead frame */
        if (getenv("RECOMP_RAMDUMP")) {      /* debug: memory image at the end of a run */
            FILE *rf = fopen(getenv("RECOMP_RAMDUMP"), "wb");
            extern uint8_t *RAM;
            if (rf) { fwrite(RAM, 1, 0x1000000, rf); fclose(rf); }
        }
        exit(0);
    }
    if (g_capture_frame && (long long)frame_counter == g_capture_frame && gpu_ok) {
        /* debug: --capture FRAME WxH FILTERS PREFIX renders the frame with each listed filter */
        extern int render_capture(const uint32_t *, int, int, int, int, const char *);
        int keep = render_settings.filter;
        for (const char *f = g_capture_filters; *f; f++) {
            render_settings.filter = (*f - '0') % RENDER_FILTERS;
            char path[700];
            snprintf(path, sizeof path, "%s_f%c.bmp", g_capture_prefix, *f);
            render_capture(fb, w, h, g_capture_w, g_capture_h, path);
        }
        render_settings.filter = keep;
    }
    /* vsync: the display's refresh paces the game.  The refresh is never
       exactly 60 Hz, so the audio queue decides: when it runs low a frame
       does not wait for the refresh, and when it is nearly full a refresh is
       skipped (one repeated frame) instead of dropping sound. */
    int vsync_frame = 0;
    if (!g_headless && gpu_ok && vsync_n && !g_turbo && !g_capture_frame) {
        static int checks;
        if (++checks % 600 == 0) update_vsync();
        int q = wo ? host_audio_queued() : 6;
        if (q >= NBUF - 1) host_sleep_until(host_time() + 1.0 / FIELD_RATE);
        vsync_frame = q >= 3;
        render_set_vsync(vsync_frame ? vsync_n : 0);
    }
    if (!g_headless && hwnd && gpu_ok && render_present(fb, w, h) != 0) {
        LOG("GPU present failed; falling back to GDI");
        gpu_ok = 0;
        update_title();
    }
    if (!g_headless && hwnd && !gpu_ok) {
        HDC dc = GetDC(hwnd);
        RECT r;
        GetClientRect(hwnd, &r);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        /* letterbox the full 384x240 (768 double-res) picture to 4:3 */
        int cw = r.right, ch = r.bottom;
        double fx, fy, fw, fh;
        picture_rect(cw, ch, &fx, &fy, &fw, &fh);
        int dx = (int)fx, dy = (int)fy, dw = (int)fw, dh = (int)fh;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (dy > 0) { RECT a = {0, 0, cw, dy}, b = {0, dy + dh, cw, ch}; FillRect(dc, &a, black); FillRect(dc, &b, black); }
        if (dx > 0) { RECT a = {0, 0, dx, ch}, b = {dx + dw, 0, cw, ch}; FillRect(dc, &a, black); FillRect(dc, &b, black); }
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, w, h, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }
    /* pacing */
    if (g_turbo || g_headless || g_capture_frame) return;
    if (vsync_frame) {
        /* the display paced this frame; fall back to the timer if Present did
           not wait (minimised, occluded) */
        static double last_vs;
        double t = host_time();
        if (t - last_vs < 0.008) host_sleep_until(last_vs + 1.0 / FIELD_RATE);
        last_vs = host_time();
        next_frame_time = last_vs;
        pi_note();
        return;
    }
    double now = host_time();
    if (next_frame_time < 0 || now - next_frame_time > 0.25) next_frame_time = now;
    next_frame_time += 1.0 / FIELD_RATE;
    host_sleep_until(next_frame_time);
    pi_note();
}

/* scripted input for automated testing: --script "field:key:dur,field:key:dur"
   keys: 1 = button 1, 2 = button 2, L R U D = directions */
typedef struct { long field; char key; int dur; } ScriptEv;
static ScriptEv script[256];
static int nscript;
void host_set_script(const char *s) {
    while (*s && nscript < 256) {
        ScriptEv *e = &script[nscript];
        e->field = strtol(s, (char **)&s, 10);
        if (*s == ':') s++;
        e->key = *s ? *s++ : '1';
        e->dur = 6;
        if (*s == ':') { s++; e->dur = (int)strtol(s, (char **)&s, 10); }
        nscript++;
        if (*s == ',') s++; else break;
    }
}

/* ---------------------------------------------------------------- XInput (loaded dynamically) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XIGamepad;
typedef struct { DWORD dwPacketNumber; XIGamepad Gamepad; } XIState;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XIState *);
static XInputGetStateFn xinput_get;
static int xinput_tried;

static void xinput_load(void) {
    xinput_tried = 1;
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xinput_get; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) xinput_get = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
    }
}

static void gamepad_poll(void) {
    if (!xinput_tried) xinput_load();
    if (!xinput_get) return;
    for (DWORD pad = 0; pad < 4; pad++) {
        XIState st;
        if (xinput_get(pad, &st) != 0) continue;
        XIGamepad *g = &st.Gamepad;
        int lx = g->sThumbLX, ly = g->sThumbLY;
        const int dead = 8000;
        if (lx > dead || lx < -dead) input.dx += (lx > 0 ? lx - dead : lx + dead) * 10 / (32767 - dead);
        if (ly > dead || ly < -dead) input.dy -= (ly > 0 ? ly - dead : ly + dead) * 10 / (32767 - dead);
        if (g->wButtons & 0x0001) input.dy -= 6;   /* dpad up */
        if (g->wButtons & 0x0002) input.dy += 6;   /* dpad down */
        if (g->wButtons & 0x0004) input.dx -= 6;   /* dpad left */
        if (g->wButtons & 0x0008) input.dx += 6;   /* dpad right */
        if (g->wButtons & 0x1000) input.b1 = 1;    /* A */
        if (g->wButtons & 0x2000) input.b2 = 1;    /* B */
        if (g->wButtons & 0x4000) { input.b1 = 1; input.b2 = 1; }  /* X: both buttons */
        if (g->wButtons & 0x0010) { input.b1 = 1; input.b2 = 1; }  /* Start: both buttons */
        if (g->wButtons & 0x8000) input.b2 = 1;    /* Y */
        break;
    }
}

void host_poll(void) {
    if (!g_headless) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    int speed = keys[VK_SHIFT] ? 12 : 6;
    input.dx = (keys[VK_RIGHT] ? speed : 0) - (keys[VK_LEFT] ? speed : 0);
    input.dy = (keys[VK_DOWN] ? speed : 0) - (keys[VK_UP] ? speed : 0);
    input.b1 = keys['Z'] || keys[VK_SPACE] || keys['A'] || keys['C'] || keys[VK_RETURN];
    input.b2 = keys['X'] || keys[VK_CONTROL] || keys['S'] || keys['C'] || keys[VK_RETURN];
    input.b3 = 0;
    input.b1 |= mouse_b1;
    input.b2 |= mouse_b2;
    input.abs = 0;
    if (mouse_moved && !g_headless) {      /* the mouse positions the pointer directly */
        input.abs = 1;
        input.x = mouse_x;
        input.y = mouse_y;
        mouse_moved = 0;
    }
    if (!g_headless) gamepad_poll();
    g_turbo = keys[VK_TAB];
    if (g_fuzz) {
        /* random input held for random durations (robustness testing) */
        static int hold, fdx, fdy, fb1, fb2;
        static uint32_t st;
        if (!st) st = (uint32_t)g_fuzz * 2654435761u + 1;
        if (--hold <= 0) {
            st = st * 1103515245u + 12345u; uint32_t r = st >> 8;
            static int calm = -1;
            if (calm < 0) calm = getenv("RECOMP_FUZZ_CALM") != NULL;   /* human-like pace */
            hold = calm ? 10 + (int)(r % 90) : 2 + (int)(r % 40);
            int dir = (int)((r >> 6) % 9);
            fdx = (dir % 3 - 1) * 6; fdy = (dir / 3 - 1) * 6;
            fb1 = ((r >> 10) % (calm ? 10 : 4)) == 0; fb2 = ((r >> 13) % (calm ? 40 : 6)) == 0;
            if (calm && (fb1 || fb2)) { fdx = fdy = 0; hold = 6; }
        }
        input.dx = fdx; input.dy = fdy; input.b1 = fb1; input.b2 = fb2;
    }
    for (int i = 0; i < nscript; i++) {
        ScriptEv *e = &script[i];
        if ((long long)frame_counter >= e->field && (long long)frame_counter < e->field + e->dur) {
            switch (e->key) {
            case '1': input.b1 = 1; break;
            case '2': input.b2 = 1; break;
            case 'L': input.dx = -6; break;
            case 'R': input.dx = 6; break;
            case 'U': input.dy = -6; break;
            case 'D': input.dy = 6; break;
            case 'S': if ((long long)frame_counter == e->field) g_req_save = 1; break;     /* quicksave */
            case 'O': if ((long long)frame_counter == e->field) g_req_load = 1; break;     /* quickload */
            }
        }
    }
    if (input.quit) {
        LOG("quit requested");
        exit(0);
    }
}

/* ---------------------------------------------------------------- audio */
#define BUF_FRAMES 1470
static WAVEHDR hdrs[NBUF];
static int16_t bufs[NBUF][BUF_FRAMES * 2];
static int cur_buf, cur_fill;

void host_audio_init(int rate) {
    if (g_headless) return;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        LOG("waveOutOpen failed; audio disabled");
        wo = NULL;
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        hdrs[i].lpData = (LPSTR)bufs[i];
        hdrs[i].dwBufferLength = BUF_FRAMES * 4;
        hdrs[i].dwFlags = WHDR_DONE;
    }
}

int host_audio_queued(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++) if (!(hdrs[i].dwFlags & WHDR_DONE)) n++;
    return n;
}

static FILE *wavf;
static long wav_frames;
void host_wav_open(const char *path) {
    wavf = fopen(path, "wb");
    if (wavf) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wavf); }
}
static void wav_close(void) {
    if (!wavf) return;
    uint32_t data = (uint32_t)wav_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + data; memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = 44100; *(uint32_t *)(h + 28) = 44100 * 4; *(uint16_t *)(h + 32) = 4;
    *(uint16_t *)(h + 34) = 16; memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = data;
    fseek(wavf, 0, SEEK_SET); fwrite(h, 1, 44, wavf); fclose(wavf); wavf = NULL;
}

void host_audio_push(const int16_t *s, int frames) {
    if (g_speculative) return;          /* run-ahead frames are not heard */
    if (wavf) {
        fwrite(s, 4, (size_t)frames, wavf);
        wav_frames += frames;
        static int reg;
        if (!reg) { reg = 1; atexit(wav_close); }
    }
    if (!wo) return;
    while (frames > 0) {
        WAVEHDR *h = &hdrs[cur_buf];
        if (!(h->dwFlags & WHDR_DONE)) return;   /* all buffers busy: drop */
        int k = BUF_FRAMES - cur_fill;
        if (k > frames) k = frames;
        memcpy(bufs[cur_buf] + cur_fill * 2, s, (size_t)k * 4);
        cur_fill += k; s += k * 2; frames -= k;
        if (cur_fill == BUF_FRAMES) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
            h->dwFlags = 0;
            h->dwBufferLength = BUF_FRAMES * 4;
            waveOutPrepareHeader(wo, h, sizeof *h);
            waveOutWrite(wo, h, sizeof *h);
            cur_buf = (cur_buf + 1) % NBUF;
            cur_fill = 0;
        }
    }
}

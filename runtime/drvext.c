/* Device driver extensions linked with SS_SLink (SetStat 0x5A).

   Some CD-i titles link a subroutine module into a device driver: the module
   gets the driver's SetStat/GetStat calls first and passes the ones it does not
   handle to the original routines.  The Balboa runtime (cdi_bpsys, used by
   The Wacky World of Miniature Golf) adds a "video environment" manager to
   /vid this way, reached through SetStat calls whose d2 is 0x42xx.  It also
   tracks the pointer and moves the cursor from a system-state alarm.

   The module's 68000 code runs in the interpreter, in a driver context built
   here, the way the OS-9 file manager calls a driver:
     a1 = path descriptor, a2 = driver static storage,
     a5 = the caller's register image, a6 = system globals.
   The driver's own routines, which the module calls through 70(a2) (SetStat)
   and 74(a2) (GetStat), are RAM stubs that trap into the HLE drivers.  Like a
   real driver they leave their results in the registers and in the register
   image at a5.

   Subroutine module entry table (at the module's exec offset, offsets relative
   to it): init, setstat, getstat, terminate (path close). */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <string.h>

#define SC_DRV_SETSTAT 0xFC     /* private os9 codes used by the RAM stubs */
#define SC_DRV_GETSTAT 0xFD
#define SC_DRV_RETURN  0xFE

#define STATIC_SIZE 0x100
#define PD_SIZE     0x100
#define REGS_SIZE   0x48        /* R$d0..R$a7 (0x00-0x3f), R$sr (0x40), R$pc (0x42) */
#define SYSGLOB_SIZE 0x400
#define D_PROC      76          /* system globals: current process descriptor */
#define D_SYSPRC    80          /*                 system process descriptor */
#define MAXDEPTH    8
#define SYSSTACK    0x1000

typedef struct {
    int linked;
    uint32_t module;            /* module header address */
    uint32_t entries[4];        /* init, setstat, getstat, term */
    uint32_t statics;           /* driver static storage (a2) */
    uint32_t stubs;             /* RAM: setstat stub, getstat stub, return stub */
    uint32_t pds[32];           /* path descriptor per path number */
    uint32_t regs[MAXDEPTH];    /* register images for nested calls */
    int depth;
} DrvExt;

static DrvExt vid_ext;
static volatile int drv_done[MAXDEPTH + 1];
static uint32_t sysglob, sysstack;

int ucm_setstat(uint32_t code);
int ucm_getstat(uint32_t code);
int ptr_setstat(uint32_t code);
int ptr_getstat(uint32_t code);
void os9_hold_signals(int delta);

int drvext_video_linked(void) { return vid_ext.linked; }

/* nonzero while 68k code runs in system state (driver routines, system alarms) */
int drvext_system_state(void) { return vid_ext.depth > 0; }

static void sysglob_init(void) {
    if (sysglob) return;
    sysglob = mem_alloc(SYSGLOB_SIZE, NULL);
    uint32_t sysprc = mem_alloc(0x200, NULL), usrprc = mem_alloc(0x200, NULL);
    WW(sysprc, 0);                                 /* P$ID */
    WW(usrprc, 1);
    WL(sysglob + D_PROC, usrprc);
    WL(sysglob + D_SYSPRC, sysprc);
    WW(sysglob + 0x0E, 100);                       /* D_TckSec */
    sysstack = mem_alloc(SYSSTACK, NULL) + SYSSTACK - 0x10;
}

static uint32_t pd_for(DrvExt *x, int path) {
    if (path < 0 || path >= 32) path = 0;
    if (!x->pds[path]) {
        x->pds[path] = mem_alloc(PD_SIZE, NULL);
        WW(x->pds[path] + 0, (uint16_t)path);      /* PD_PD */
        WB(x->pds[path] + 2, 3);                   /* PD_MOD: read+write */
    }
    return x->pds[path];
}

static void regs_to_image(uint32_t img) {
    for (int i = 0; i < 8; i++) WL(img + i * 4, D(i));
    for (int i = 0; i < 8; i++) WL(img + 0x20 + i * 4, A(i));
    WW(img + 0x40, GET_SR());
    WL(img + 0x42, m68k.pc);
}

static void restore_cpu(const M68K *saved) {
    int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
    int y = m68k.yield;
    m68k = *saved;
    m68k.cyc = cyc; m68k.cyc_limit = lim; m68k.yield = y;
}

/* Run 68k code at 'entry' as a driver routine until it returns (rts).  The
   caller has raised x->depth and set up the registers. */
static void run_driver_routine(DrvExt *x, uint32_t entry) {
    int lvl = x->depth - 1;
    A(6) = sysglob;
    A(7) -= 4;
    WL(A(7), x->stubs + 12);                       /* return into the 'os9 #$FE' stub */
    m68k.pc = entry;
    m68k.srh = 0x20;                               /* supervisor */
    drv_done[lvl] = 0;
    os9_hold_signals(1);                           /* no user signals in system state */
    cpu_run_until(&drv_done[lvl]);
    os9_hold_signals(-1);
}

/* Called by the runtime's os9 dispatcher for the private stub codes. */
uint32_t drvext_syscall(uint32_t code, uint32_t next) {
    DrvExt *x = &vid_ext;
    if (code == SC_DRV_RETURN) {
        drv_done[x->depth > 0 ? x->depth - 1 : 0] = 1;
        return 0xFFFFFFFCu;                        /* stops the interpreter */
    }
    /* the original driver's SetStat/GetStat: d1 = code, d2.. = parameters as
       for I$SetStt/I$GetStt */
    uint32_t img = A(5);
    uint32_t save_d[8], save_a[8];
    memcpy(save_d, m68k.d, sizeof save_d);
    memcpy(save_a, m68k.a, sizeof save_a);
    uint32_t c = D(1) & 0xFFFF;
    int e;
    if (code == SC_DRV_SETSTAT) e = c == 0x59 ? ptr_setstat(c) : ucm_setstat(c);
    else e = c == 0x59 ? ptr_getstat(c) : ucm_getstat(c);
    if (img) {
        for (int i = 0; i < 8; i++)
            if (m68k.d[i] != save_d[i]) WL(img + i * 4, m68k.d[i]);
        for (int i = 0; i < 7; i++)
            if (m68k.a[i] != save_a[i]) WL(img + 0x20 + i * 4, m68k.a[i]);
    }
    if (e) { D(1) = (D(1) & 0xFFFF0000u) | (uint32_t)e; CF = 1; }
    else CF = 0;
    return next;
}

/* SS_SLink on the video path: a0 = module name.  Returns 0 or an error. */
int drvext_video_slink(uint32_t name_addr, int path) {
    extern uint32_t os9_find_module(const char *name);
    char nm[64];
    int i = 0;
    for (; i < 63; i++) {
        uint8_t c = RB(name_addr + i);
        if (c <= ' ') break;
        nm[i] = (char)c;
    }
    nm[i] = 0;
    uint32_t mb = os9_find_module(nm);
    if (!mb) { LOG("SS_SLink: module '%s' not found", nm); return 0xDD; }
    DrvExt *x = &vid_ext;
    if (x->linked && x->module == mb) return 0;
    sysglob_init();
    memset(x, 0, sizeof *x);
    x->module = mb;
    uint32_t tbl = mb + RL(mb + 0x30);
    for (int k = 0; k < 4; k++) x->entries[k] = tbl + RW(tbl + k * 2);
    x->statics = mem_alloc(STATIC_SIZE, NULL);
    x->stubs = mem_alloc(16, NULL);
    /* +0: os9 #$FC ; rts   (driver SetStat)
       +6: os9 #$FD ; rts   (driver GetStat)
       +12: os9 #$FE        (end of a driver routine run from the runtime) */
    WW(x->stubs + 0, 0x4E40); WW(x->stubs + 2, SC_DRV_SETSTAT); WW(x->stubs + 4, 0x4E75);
    WW(x->stubs + 6, 0x4E40); WW(x->stubs + 8, SC_DRV_GETSTAT); WW(x->stubs + 10, 0x4E75);
    WW(x->stubs + 12, 0x4E40); WW(x->stubs + 14, SC_DRV_RETURN);
    WL(x->statics + 70, x->stubs);
    WL(x->statics + 74, x->stubs + 6);
    for (int k = 0; k < MAXDEPTH; k++) x->regs[k] = mem_alloc(REGS_SIZE, NULL);
    x->linked = 1;
    LOG("SS_SLink '%s' at %06x: init %06x setstat %06x getstat %06x term %06x", nm, mb,
        x->entries[0], x->entries[1], x->entries[2], x->entries[3]);
    M68K saved = m68k;
    regs_to_image(x->regs[0]);
    A(1) = pd_for(x, path);
    A(2) = x->statics;
    A(5) = x->regs[0];
    A(7) = sysstack;
    x->depth = 1;
    run_driver_routine(x, x->entries[0]);
    x->depth = 0;
    int err = CF ? (int)(D(1) & 0xFFFF) : 0;
    restore_cpu(&saved);
    if (err) LOG("SS_SLink '%s': init returned error %x", nm, err);
    return err;
}

/* System stack for nested driver calls: below whatever is in use. */
static uint32_t driver_stack(void) {
    if (vid_ext.depth == 0) return sysstack;
    return A(7) - 0x100;
}

/* SetStat on the video path through the linked module.  Returns -1 when the
   call does not concern the extension (the caller then runs the HLE driver
   directly, which is what the module's pass-through does), otherwise 0 or an
   error code; the caller's registers are reloaded from the register image. */
int drvext_video_setstat(int path) {
    DrvExt *x = &vid_ext;
    if (!x->linked) return -1;
    uint16_t fn = (uint16_t)D(2);
    if (fn < 0x4200 || fn >= 0x4214) return -1;
    if (x->depth >= MAXDEPTH) fatal("driver extension nesting too deep");
    uint32_t img = x->regs[x->depth];
    M68K saved = m68k;
    regs_to_image(img);
    A(1) = pd_for(x, path);
    A(2) = x->statics;
    A(4) = RL(sysglob + D_PROC);
    A(5) = img;
    A(7) = driver_stack();
    x->depth++;
    run_driver_routine(x, x->entries[1]);
    x->depth--;
    int err = CF ? (int)(D(1) & 0xFFFF) : 0;
    if (!err && CF) err = 1;
    restore_cpu(&saved);
    for (int i = 0; i < 8; i++) D(i) = RL(img + i * 4);
    for (int i = 0; i < 7; i++) A(i) = RL(img + 0x20 + i * 4);
    TRACE("bpsys setstat %x fn %x -> err %x d0=%08x", saved.d[1] & 0xFFFF, fn, err, D(0));
    return err;
}

/* I$Close of a video path: let the module drop what belongs to it. */
void drvext_video_close(int path) {
    DrvExt *x = &vid_ext;
    if (!x->linked || path < 0 || path >= 32 || !x->pds[path]) return;
    if (x->depth >= MAXDEPTH) return;
    M68K saved = m68k;
    uint32_t img = x->regs[x->depth];
    regs_to_image(img);
    A(1) = x->pds[path];
    A(2) = x->statics;
    A(5) = img;
    A(7) = driver_stack();
    x->depth++;
    run_driver_routine(x, x->entries[3]);
    x->depth--;
    restore_cpu(&saved);
}

/* ------------------------------------------------------------ system alarms
   F$Alarm made in system state runs a routine instead of sending a signal:
   a0 points to a register image whose R$pc is the routine and R$sr its status
   register; the routine returns with rts. */
void drvext_run_system_alarm(uint32_t img) {
    DrvExt *x = &vid_ext;
    if (x->depth >= MAXDEPTH) return;
    M68K saved = m68k;
    for (int i = 0; i < 8; i++) D(i) = RL(img + i * 4);
    for (int i = 0; i < 7; i++) A(i) = RL(img + 0x20 + i * 4);
    A(7) = driver_stack();
    uint32_t pc = RL(img + 0x42);
    SET_CCR(RW(img + 0x40));
    x->depth++;
    int64_t c0 = m68k.cyc;
    run_driver_routine(x, pc);
    x->depth--;
    if (log_level >= 3) TRACE("system alarm %06x: %lld cycles", pc, (long long)(m68k.cyc - c0));
    restore_cpu(&saved);
}

/* ------------------------------------------------------------ snapshots */
void drvext_state(StateIO *s) {
    ST(s, vid_ext);
    ST(s, sysglob);
    ST(s, sysstack);
}

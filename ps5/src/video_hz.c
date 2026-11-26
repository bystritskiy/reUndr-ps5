/* reUndr PS5, MIT. 119.88 Hz output, VRR displays included.
 *
 * PS5_Mesa (47e9eb6, wsi_common_videoout.c) configures VideoOut's 119.88 Hz
 * mode when it opens VideoOut and keeps it only if a vblank then comes within
 * 12.5 ms. It measures before anything is flipped, and a VRR display
 * refreshes at its floor while nothing is: on a 4K TV with a 48-120 Hz range
 * the console logged "119.88Hz request is converted to VRR 119.88Hz", Mesa
 * measured 20.87 ms (48 Hz) and restored 59.94 Hz, at every start
 * (console, 2026-11-26).
 *
 * tools/link.sh wraps Mesa's sceVideoOutOpen and sceVideoOutConfigureOutput.
 * A restore right after VideoOut took 119.88 Hz is held. Once the title
 * draws, the title flips at up to 120 fps for a moment (nfsu1_ps5_output_check)
 * and the vblank is measured again: within 12.5 ms the output stays at
 * 119.88 Hz, otherwise Mesa's restore is carried out. Mesa itself still
 * reports 59.94 Hz then, so nfsu1_ps5_output_mhz is the output's refresh.
 * NFSU1_VIDEO_HZ=60 in nfsu1_env.txt lets Mesa's restore through at once.
 *
 * Mesa reads the title's 119.88 Hz flag (attribute3 0x40) from
 * /app0/sce_sys/param.json, which an installed package does not have: the
 * system keeps it where the title cannot read it, so Mesa never asked for
 * 119.88 Hz (console, FPKG, 2026-11-26). Then a file declaring the flag
 * stands in through PS5_VIDEOOUT_PARAM_JSON until VideoOut is open. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

extern int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
extern int __real_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void *, const void *,
                                             const void *);
extern int sceVideoOutWaitVblank(int32_t handle);
extern uint32_t nv2a_vk_display_mhz;            /* nv2a_vk.c: Mesa's fastest mode */
extern volatile unsigned nv2a_vk_frame_draws;

#define MODE_HIGH_FRAME_RATE 15u
#define MODE_RESTORE 1u
#define HIGH_LIMIT_NS UINT64_C(12500000)

/* The output's refresh in millihertz once known (119880 or 59940), 0 before. */
volatile uint32_t nfsu1_ps5_output_mhz;
/* 1 while the held 119.88 Hz is being checked: the title flips at up to 120. */
volatile int nfsu1_ps5_output_check;
/* 1 when the 119.88 Hz output is the held one (a VRR display, by its floor). */
volatile int nfsu1_ps5_output_vrr;

static volatile int s_handle = -1;
static volatile int s_high_taken, s_restore_held;

#define PARAM_STAND_IN "/data/reUndr/videoout.json"
static int s_stand_in;

int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter)
{
    int h = __real_sceVideoOutOpen(user, bus, index, parameter);
    if (h >= 0)
        s_handle = h;
    return h;
}

int __wrap_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void *a, const void *b, const void *c)
{
    if (mode == MODE_RESTORE && s_high_taken && !s_restore_held) {
        const char *e = getenv("NFSU1_VIDEO_HZ");
        if (!(e && atoi(e) == 60)) {
            s_restore_held = 1;
            return 0;
        }
    }
    int rc = __real_sceVideoOutConfigureOutput(handle, mode, a, b, c);
    if (mode == MODE_HIGH_FRAME_RATE)
        s_high_taken = rc == 0;
    else if (rc == 0)
        s_high_taken = 0;
    return rc;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* The median of 15 intervals between vblanks; 0 when a wait fails. */
static uint64_t vblank_period_ns(int h)
{
    uint64_t d[15], prev;
    if (sceVideoOutWaitVblank(h) != 0)
        return 0;
    prev = now_ns();
    for (int i = 0; i < 15; i++) {
        uint64_t t;
        if (sceVideoOutWaitVblank(h) != 0)
            return 0;
        t = now_ns();
        d[i] = t - prev;
        prev = t;
    }
    for (int i = 1; i < 15; i++)
        for (int j = i; j > 0 && d[j - 1] > d[j]; j--) {
            uint64_t s = d[j]; d[j] = d[j - 1]; d[j - 1] = s;
        }
    return d[7];
}

static void *video_hz_thread(void *arg)
{
    (void)arg;
    /* The swapchain made (after Mesa's open and mode list) and the title drawing. */
    while (!nv2a_vk_display_mhz || !nv2a_vk_frame_draws)
        usleep(100000);
    if (s_stand_in)
        unlink(PARAM_STAND_IN);
    if (!s_restore_held) {
        nfsu1_ps5_output_mhz = nv2a_vk_display_mhz;
        fprintf(stderr, "[video] output %.3f Hz\n", nv2a_vk_display_mhz / 1000.0);
        return NULL;
    }
    nfsu1_ps5_output_check = 1;
    sleep(1);
    uint64_t period = vblank_period_ns(s_handle);
    int kept = period && period <= HIGH_LIMIT_NS;
    if (!kept) {
        s_restore_held = 0;
        __real_sceVideoOutConfigureOutput(s_handle, MODE_RESTORE, NULL, NULL, NULL);
        s_high_taken = 0;
    }
    nfsu1_ps5_output_vrr = kept;
    nfsu1_ps5_output_mhz = kept ? 119880 : 59940;
    nfsu1_ps5_output_check = 0;
    fprintf(stderr, "[video] 119.88 Hz held past Mesa's check: vblank %.3f ms while flipping, %s\n",
            period / 1e6, kept ? "output 119.880 Hz (VRR)" : "59.94 Hz restored");
    return NULL;
}

void nfsu1_ps5_video_hz_start(void)
{
    pthread_t t;
    if (!getenv("PS5_VIDEOOUT_PARAM_JSON") && access("/app0/sce_sys/param.json", R_OK) != 0) {
        FILE *f = fopen(PARAM_STAND_IN, "w");
        if (f) {
            fputs("{\"attribute3\": 64}\n", f);
            s_stand_in = fclose(f) == 0;
            if (s_stand_in)
                setenv("PS5_VIDEOOUT_PARAM_JSON", PARAM_STAND_IN, 1);
        }
    }
    if (pthread_create(&t, NULL, video_hz_thread, NULL) == 0)
        pthread_detach(t);
}

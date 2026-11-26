/* reUndr PS5, MIT. A knock on the pad at each gear change.
 *
 * recomp_manual.c reads the player car's gear each race frame (the car's
 * simulation, +0x3D8: 0 reverse, 1 first ...). When it changes, pad 1 gets a
 * 70 ms pulse of half strength on both motors over the title's own rumble,
 * scaled by Vibration. */
#include <stdint.h>
#include <string.h>

extern uint64_t recomp_now_ns(void);    /* win32_compat.c: the fast clock */
extern int scePadSetVibration(int, const void *);
extern int nfsu1_in_race(void) __attribute__((weak));   /* recomp_manual.c */
extern volatile int nfsu1_car_gear;
extern volatile uint32_t nfsu1_race_frames;
extern volatile int nfsu1_vibration_pct;

#define PORTS     2
#define PULSE_NS  70000000ull
#define PULSE     0x80                  /* of 255, before Vibration */

typedef struct {
    int handle, gear, pulsing;
    uint32_t frames;
    uint64_t seen_ns, pulse_until;
} PadState;
static PadState s_pads[PORTS];
static unsigned s_game_motor[PORTS][2];   /* the title's last motor bytes */

/* The motor bytes input_ps5.c sent for the title, to return to after a pulse. */
void nfsu1_gear_pulse_motors(unsigned port, uint8_t large, uint8_t small)
{
    if (port < PORTS) {
        s_game_motor[port][0] = large;
        s_game_motor[port][1] = small;
    }
}

/* Each read of pad `port` (under input_ps5.c's lock). */
void nfsu1_gear_pulse_update(unsigned port, int handle, int menu_open)
{
    PadState *p;
    uint64_t now = recomp_now_ns();
    if (port >= PORTS || handle < 0)
        return;
    p = &s_pads[port];
    if (p->handle != handle) {
        memset(p, 0, sizeof *p);
        p->handle = handle;
        p->gear = -1;
    }
    uint32_t frames = nfsu1_race_frames;
    if (frames != p->frames) {
        p->frames = frames;
        p->seen_ns = now;
    }
    int racing = port == 0 && nfsu1_in_race && nfsu1_in_race() && now - p->seen_ns < 250000000ull;
    int gear = racing ? nfsu1_car_gear : -1;
    if (gear >= 0 && p->gear >= 0 && gear != p->gear && !menu_open && nfsu1_vibration_pct > 0)
        p->pulse_until = now + PULSE_NS;
    p->gear = gear;
    int pulse = now < p->pulse_until;
    if (pulse || p->pulsing) {
        unsigned k = (unsigned)(PULSE * nfsu1_vibration_pct / 100);
        unsigned l = s_game_motor[port][0], r = s_game_motor[port][1];
        uint8_t m[2] = { (uint8_t)(pulse && l < k ? k : l), (uint8_t)(pulse && r < k ? k : r) };
        scePadSetVibration(handle, m);
        p->pulsing = pulse;
    }
}

/* RECOMP_FAST_TIME clock (platform/fast_time.h): off by default; on, 4 threads
 * for 2 s never see it step back and never find it more than 200 us from a
 * clock_gettime taken around the read. Then rest-mode discontinuities through
 * a fake TSC (FT_TEST_TSC): reset back 10 s, +60 s against 1 s of
 * CLOCK_MONOTONIC, stopped for 1 s of CLOCK_MONOTONIC. A second thread reads
 * all along: the clock never steps back, never runs ahead of the real time by
 * more than 2 anchors (so it cannot hold still for long), catches up within
 * 2 reads an anchor apart, and is back within 200 us right after. */
#include <stdint.h>
#if defined(__x86_64__) || defined(__i386__)
static uint64_t fake_off;               /* added to the real TSC */
static uint64_t fake_frozen;            /* nonzero: the TSC stands here */
static uint64_t fake_tsc(void)
{
    uint64_t f = __atomic_load_n(&fake_frozen, __ATOMIC_SEQ_CST);
    return f ? f : __builtin_ia32_rdtsc() + __atomic_load_n(&fake_off, __ATOMIC_SEQ_CST);
}
#  define FT_TEST_TSC fake_tsc
#endif
#include "platform/fast_time.h"
#include <assert.h>
#include <pthread.h>

#define THREADS 4
#define RUN_NS 2000000000ull
#define MAX_ERR_NS 200000ull

typedef struct { uint64_t reads, backsteps, max_err; } Result;

static void *worker(void *arg)
{
    Result *r = arg;
    uint64_t start = ft_mono_ns(CLOCK_MONOTONIC), prev = 0;
    for (;;) {
        uint64_t r0 = ft_mono_ns(CLOCK_MONOTONIC), f = ft_now_ns(), r1 = ft_mono_ns(CLOCK_MONOTONIC), err, i;
        err = f < r0 ? r0 - f : f > r1 ? f - r1 : 0;
        if (err > r->max_err) r->max_err = err;
        if (f < prev) r->backsteps++;
        prev = f;
        for (i = 0; i < 200; i++) {               /* the hot path, without the reference reads */
            f = ft_now_ns();
            if (f < prev) r->backsteps++;
            prev = f;
        }
        r->reads += 201;
        if (r1 - start >= RUN_NS) return NULL;
    }
}

#if defined(FT_TEST_TSC)
typedef struct { int stop; uint64_t reads, backsteps, max_ahead; } Hammer;

static void *hammer(void *arg)
{
    Hammer *h = arg;
    uint64_t prev = 0;
    while (!__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE)) {
        uint64_t f = ft_now_ns(), r1 = ft_mono_ns(CLOCK_MONOTONIC);
        if (f < prev) h->backsteps++;
        if (f > r1 && f - r1 > h->max_ahead) h->max_ahead = f - r1;
        prev = f;
        h->reads++;
    }
    return NULL;
}

static void sleep_ns(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
}

/* Rest mode: the TSC stands while CLOCK_MONOTONIC advances `mono_ns`, then
 * resumes `tsc_ns` (signed) from where it stopped. */
static void suspend(int64_t tsc_ns, uint64_t mono_ns)
{
    uint64_t scale = ft_a[__atomic_load_n(&ft_cur, __ATOMIC_ACQUIRE) & 1].scale;
    uint64_t f = fake_tsc();
    int64_t ticks = tsc_ns < 0 ? -(int64_t)(((unsigned __int128)-tsc_ns << 32) / scale)
                               : (int64_t)(((unsigned __int128)tsc_ns << 32) / scale);
    __atomic_store_n(&fake_frozen, f, __ATOMIC_SEQ_CST);
    sleep_ns(mono_ns);
    __atomic_store_n(&fake_off, f + (uint64_t)ticks - __builtin_ia32_rdtsc(), __ATOMIC_SEQ_CST);
    __atomic_store_n(&fake_frozen, 0, __ATOMIC_SEQ_CST);
}

static void discontinuity(const char *name, int64_t tsc_ns, uint64_t mono_ns)
{
    uint64_t d0 = __atomic_load_n(&ft_n_disc, __ATOMIC_RELAXED), max_err = 0, end, prev = 0;
    int reads = 0, settled = 0;

    suspend(tsc_ns, mono_ns);
    while (!settled) {                            /* reads an anchor period apart */
        uint64_t r0 = ft_mono_ns(CLOCK_MONOTONIC), f = ft_now_ns(), r1 = ft_mono_ns(CLOCK_MONOTONIC);
        assert(f >= prev);
        prev = f;
        settled = f + 2 * FT_ANCHOR_NS >= r0 && f <= r1 + 2 * FT_ANCHOR_NS;
        if (++reads > 2) {
            printf("fast_time: %s: %d reads, fast-real %+.1f us\n", name, reads, ((double)f - (double)r1) / 1000.0);
            assert(!"not caught up within 2 reads");
        }
        if (!settled) sleep_ns(FT_ANCHOR_NS + FT_ANCHOR_NS / 4);
    }
    sleep_ns(3 * FT_ANCHOR_NS);
    end = ft_mono_ns(CLOCK_MONOTONIC) + 50000000ull;
    for (;;) {                                    /* 50 ms of bracketed reads */
        uint64_t r0 = ft_mono_ns(CLOCK_MONOTONIC), f = ft_now_ns(), r1 = ft_mono_ns(CLOCK_MONOTONIC);
        uint64_t err = f < r0 ? r0 - f : f > r1 ? f - r1 : 0;
        assert(f >= prev);
        prev = f;
        if (err > max_err) max_err = err;
        if (r1 >= end) break;
    }
    printf("fast_time: %s: caught up after %d read(s), then max err %.1f us, discontinuities +%llu\n", name, reads,
           max_err / 1000.0, (unsigned long long)(__atomic_load_n(&ft_n_disc, __ATOMIC_RELAXED) - d0));
    assert(max_err < MAX_ERR_NS);
    assert(__atomic_load_n(&ft_n_disc, __ATOMIC_RELAXED) > d0);
}
#endif

int main(void)
{
    pthread_t th[THREADS];
    Result res[THREADS] = {{0}};
    uint64_t reads = 0, backsteps = 0, max_err = 0, anchors;
    int i;

    unsetenv("RECOMP_FAST_TIME");
    ft_init();
    assert(!ft_active());                          /* default: off */
    ft_init();
    assert(ft_on == 0);                            /* once */

    ft_on = -1;
    setenv("RECOMP_FAST_TIME", "1", 1);
    ft_init();
    assert(ft_active() == FT_HAVE_TSC);
    if (!FT_HAVE_TSC) { puts("fast_time: no TSC, skipped"); return 0; }

    for (i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, &res[i]);
    for (i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        reads += res[i].reads;
        backsteps += res[i].backsteps;
        if (res[i].max_err > max_err) max_err = res[i].max_err;
    }
    anchors = ft_n_anchor + ft_n_skip;
    ft_report(stdout, ft_a[ft_cur & 1].scale);
    printf("fast_time: %d threads, %llu reads, %llu anchors, max err %.1f us, backsteps %llu\n", THREADS,
           (unsigned long long)reads, (unsigned long long)anchors, max_err / 1000.0, (unsigned long long)backsteps);
    assert(backsteps == 0);
    assert(max_err < MAX_ERR_NS);
    assert(anchors >= RUN_NS / FT_ANCHOR_NS / 2);  /* re-anchored all along */
    {   /* Wall = offset read at init + this clock; realtime may since have been
         * stepped (WSL does), so this is printed, not asserted. */
        uint64_t w0 = ft_mono_ns(CLOCK_REALTIME), w = ft_wall_ns();
        printf("fast_time: wall - realtime %+.1f us\n", ((double)w - (double)w0) / 1000.0);
    }
#if defined(FT_TEST_TSC)
    {
        Hammer h = {0};
        pthread_t ht;
        pthread_create(&ht, NULL, hammer, &h);
        discontinuity("TSC back 10 s", -10000000000ll, 10000000ull);
        discontinuity("TSC +60 s, monotonic +1 s", 60000000000ll, 1000000000ull);
        discontinuity("TSC stopped, monotonic +1 s", 0, 1000000000ull);
        __atomic_store_n(&h.stop, 1, __ATOMIC_RELEASE);
        pthread_join(ht, NULL);
        ft_report(stdout, ft_a[ft_cur & 1].scale);
        printf("fast_time: reader thread %llu reads, backsteps %llu, max ahead of real %.1f us\n",
               (unsigned long long)h.reads, (unsigned long long)h.backsteps, h.max_ahead / 1000.0);
        assert(h.backsteps == 0);
        assert(h.max_ahead <= 2 * FT_ANCHOR_NS);
    }
#endif
    puts("fast_time: ok");
    return 0;
}

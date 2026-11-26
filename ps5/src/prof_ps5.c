/* reUndr PS5, MIT. RECOMP_PROF=<start_s>[:<length_s>]: a sampling profile of
 * the threads that set the frame rate, for finding where frame time goes.
 *
 * Threads register themselves (nfsu1_prof_register: the guest main thread,
 * the pushbuffer executor, the Vulkan recording thread). From start_s after
 * boot for length_s (default 60), a thread of ours signals each of them
 * (SIGUSR2) every millisecond; the handler stores the interrupted RIP. At
 * the end /data/reUndr/prof.bin holds a header (magic, the runtime address
 * of nfsu1_prof_register, the thread names) and the samples (tag, RIP), to
 * be matched against llvm-pie.elf's symbols on the computer. Off unless
 * RECOMP_PROF is set; nothing is signalled then. */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ucontext.h>

#define PROF_THREADS 8
#define PROF_SAMPLES (1u << 21)

static pthread_t s_thread[PROF_THREADS];
static char s_name[PROF_THREADS][16];
static volatile int s_count;
static uint64_t *s_rip;
static uint8_t *s_tag;
static volatile uint32_t s_n;
static __thread int t_tag = -1;
static int s_on = -1;

static void on_sample(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)info;
    if (t_tag < 0 || !s_rip)
        return;
    uint32_t i = __atomic_fetch_add(&s_n, 1, __ATOMIC_RELAXED);
    if (i < PROF_SAMPLES) {
        s_rip[i] = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.mc_rip;
        s_tag[i] = (uint8_t)t_tag;
    }
}

static int prof_on(void)
{
    if (s_on < 0)
        s_on = getenv("RECOMP_PROF") != NULL;
    return s_on;
}

void nfsu1_prof_register(const char *name)
{
    if (!prof_on())
        return;
    int i = __atomic_fetch_add(&s_count, 1, __ATOMIC_ACQ_REL);
    if (i >= PROF_THREADS)
        return;
    s_thread[i] = pthread_self();
    snprintf(s_name[i], sizeof s_name[i], "%s", name);
    t_tag = i;
    fprintf(stderr, "[prof] thread %d: %s\n", i, name);
}

static void *prof_thread(void *arg)
{
    (void)arg;
    const char *e = getenv("RECOMP_PROF");
    unsigned start = (unsigned)atoi(e), length = 60;
    const char *colon = strchr(e, ':');
    if (colon)
        length = (unsigned)atoi(colon + 1);
    sleep(start);
    fprintf(stderr, "[prof] sampling %u threads for %u s\n", (unsigned)s_count, length);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        int n = s_count < PROF_THREADS ? s_count : PROF_THREADS;
        for (int i = 0; i < n; i++)
            pthread_kill(s_thread[i], SIGUSR2);
        usleep(1000);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((unsigned)(now.tv_sec - t0.tv_sec) < length);
    uint32_t total = s_n < PROF_SAMPLES ? s_n : PROF_SAMPLES;
    FILE *f = fopen("/data/reUndr/prof.bin", "wb");
    if (f) {
        uint64_t head[2] = {0x464f525055444e52ull, (uint64_t)(uintptr_t)&nfsu1_prof_register};
        uint32_t nt = (uint32_t)(s_count < PROF_THREADS ? s_count : PROF_THREADS);
        fwrite(head, sizeof head, 1, f);
        fwrite(&nt, 4, 1, f);
        fwrite(s_name, sizeof s_name[0], PROF_THREADS, f);
        fwrite(&total, 4, 1, f);
        fwrite(s_rip, 8, total, f);
        fwrite(s_tag, 1, total, f);
        fclose(f);
    }
    fprintf(stderr, "[prof] %u samples written to /data/reUndr/prof.bin\n", total);
    return NULL;
}

void nfsu1_prof_start(void)
{
    if (!prof_on())
        return;
    s_rip = (uint64_t *)malloc(PROF_SAMPLES * sizeof *s_rip);
    s_tag = (uint8_t *)malloc(PROF_SAMPLES);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, prof_thread, NULL) == 0)
        pthread_detach(t);
}

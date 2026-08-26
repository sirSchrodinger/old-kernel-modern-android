/* Memory latency by pointer chase.
 *
 * FIRST VERSION READ ZERO AT EVERY SIZE - it was not measuring anything.  The
 * chain was built through a plain size_t* and gcc at -O2 was free to keep the
 * chase in registers instead of going to memory each step, so the "latency"
 * came out at 0.01 ns, i.e. a hundredth of a cycle.  A number that is
 * physically impossible is not a small error, it is the measurement not
 * happening - so the loads are now through a volatile pointer, which the
 * compiler may not hoist, and the result is checked against one cycle before
 * it is printed.
 *
 * Each step depends on the previous load, so the core cannot overlap them and
 * what comes out is the real round trip to whichever level of the hierarchy
 * the working set lands in.  Sweeping the size draws the cache boundaries: on
 * this SoC 32 KB L1 and 512 KB L2 should show up as two steps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double simdi(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* 64 bytes: one cache line on this core, so consecutive nodes never share a
 * line and every step is a real miss once the set is bigger than the level. */
#define SATIR 64

int main(int argc, char **argv) {
    int mhz = argc > 1 ? atoi(argv[1]) : 0;
    size_t boyutlar[] = {4096, 16384, 32768, 65536, 131072, 262144, 524288,
                         1048576, 2097152, 8388608, 33554432};
    int n = (int)(sizeof boyutlar / sizeof boyutlar[0]);
    printf("# isaretci kovalama - %d B satir, rastgele sira, volatile yuk\n", SATIR);
    if (mhz) printf("# %d MHz varsayilarak cevrim de yaziliyor\n", mhz);
    printf("# %-10s %-12s %-10s %s\n", "boyut", "gecikme", "cevrim", "beklenen");
    srand(1);
    for (int s = 0; s < n; s++) {
        size_t boyut = boyutlar[s];
        size_t adet = boyut / SATIR;
        if (adet < 4) continue;

        char *ham = calloc(1, boyut + SATIR);
        size_t *sira = malloc(adet * sizeof(size_t));
        if (!ham || !sira) { printf("%-10zu  ayrilamadi\n", boyut); free(ham); free(sira); continue; }
        for (size_t i = 0; i < adet; i++) sira[i] = i;
        /* Fisher-Yates: a random walk defeats the stride prefetcher, which a
         * forward chain would not - a prefetched chase measures bandwidth, not
         * latency, and that is a different question. */
        for (size_t i = adet - 1; i > 0; i--) {
            size_t j = (size_t)(rand() % (int)(i + 1));
            size_t t = sira[i]; sira[i] = sira[j]; sira[j] = t;
        }
        for (size_t i = 0; i < adet; i++) {
            void **dugum = (void **)(ham + sira[i] * SATIR);
            *dugum = (void *)(ham + sira[(i + 1) % adet] * SATIR);
        }

        void *volatile *p = (void *volatile *)(ham + sira[0] * SATIR);
        size_t hedef = 8000000;                 /* steps, not iterations */
        size_t tur = hedef / adet; if (tur < 2) tur = 2;
        size_t adim = tur * adet;

        for (size_t i = 0; i < adet; i++) p = (void *volatile *)*p;   /* warm */
        double t0 = simdi();
        for (size_t r = 0; r < tur; r++)
            for (size_t i = 0; i < adet; i++) p = (void *volatile *)*p;
        double dt = simdi() - t0;

        double ns = dt / (double)adim * 1e9;
        const char *bek = boyut <= 32768 ? "L1" : (boyut <= 524288 ? "L2" : "DRAM");
        if (ns < 0.3)
            printf("%-10zu  OLCULEMEDI (%.3f ns - derleyici kaldirmis olmali)\n", boyut, ns);
        else if (mhz)
            printf("%-10zu %8.2f ns   %7.1f    %s\n", boyut, ns, ns * mhz / 1000.0, bek);
        else
            printf("%-10zu %8.2f ns   %7s    %s\n", boyut, ns, "-", bek);
        fflush(stdout);
        if (p == (void *volatile *)1) printf("");
        free(sira); free(ham);
    }
    return 0;
}

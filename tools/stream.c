/* STREAM, trimmed to what this handset can answer.
 *
 * The question it is here for: ncnn on this SoC gains only 1.27x from the
 * second core.  On a dual Cortex-A9 a well-threaded convolution should reach
 * 1.7-1.8x, so either the cores are not both running or the memory system is
 * the ceiling.  STREAM separates those: if one thread already saturates the
 * bus, two threads will show almost no gain here either, and the 1.27x is
 * physics rather than a software bug.
 *
 * Array size: the u8500 has 512 KB of L2.  16 MB per array is 32x that, so
 * nothing survives in cache between kernels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define N 2000000
#define TEKRAR 10

static double a[N], b[N], c[N];

static double simdi(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void) {
    double en_iyi[4] = {1e30, 1e30, 1e30, 1e30};
    const char *ad[4] = {"Copy", "Scale", "Add", "Triad"};
    /* bytes moved per element, per the STREAM definition */
    const double bayt[4] = {2.0 * sizeof(double), 2.0 * sizeof(double),
                            3.0 * sizeof(double), 3.0 * sizeof(double)};
    int i, k;

#pragma omp parallel for
    for (i = 0; i < N; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }

    for (k = 0; k < TEKRAR; k++) {
        double t;
        t = simdi();
#pragma omp parallel for
        for (i = 0; i < N; i++) c[i] = a[i];
        t = simdi() - t; if (t < en_iyi[0]) en_iyi[0] = t;

        t = simdi();
#pragma omp parallel for
        for (i = 0; i < N; i++) b[i] = 3.0 * c[i];
        t = simdi() - t; if (t < en_iyi[1]) en_iyi[1] = t;

        t = simdi();
#pragma omp parallel for
        for (i = 0; i < N; i++) c[i] = a[i] + b[i];
        t = simdi() - t; if (t < en_iyi[2]) en_iyi[2] = t;

        t = simdi();
#pragma omp parallel for
        for (i = 0; i < N; i++) a[i] = b[i] + 3.0 * c[i];
        t = simdi() - t; if (t < en_iyi[3]) en_iyi[3] = t;
    }

    printf("# STREAM  N=%d  dizi=%.1f MB  tekrar=%d\n",
           N, N * sizeof(double) / 1048576.0, TEKRAR);
#ifdef _OPENMP
    printf("# is parcacigi: %d\n", omp_get_max_threads());
#endif
    for (k = 0; k < 4; k++)
        printf("%-6s %10.1f MB/s   %8.5f s\n", ad[k],
               bayt[k] * N / en_iyi[k] / 1e6, en_iyi[k]);
    /* keep the arrays alive so the optimiser cannot delete the whole thing */
    if (a[N / 2] == 12345.6789) printf("");
    return 0;
}

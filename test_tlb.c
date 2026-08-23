#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define PAGE 4096

int main(int argc, char **argv)
{
    int pages = atoi(argv[1]);
    char *buf = malloc((long)pages * PAGE);
    for (long i = 0; i < (long)pages * PAGE; i += PAGE)
        buf[i] = 1;                          /* 先踩一遍:把 page fault 排除在計時外 */

    long total = 2000000, n = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (n < total)
        for (int p = 0; p < pages && n < total; p++, n++)
            buf[(long)p * PAGE + (p % 128) * 32] += 1;       /* 每次存取都換頁 */
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = ((t1.tv_sec - t0.tv_sec) * 1e9
              + (t1.tv_nsec - t0.tv_nsec)) / total;
    printf("%4d pages: %6.2f ns/access\n", pages, ns);
    return 0;
}
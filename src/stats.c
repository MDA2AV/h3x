/* Latency sample collection and the end-of-run summary (merged percentiles). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h3x.h"

void record_latency(struct worker *w, const struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t us = (uint64_t)(now.tv_sec - start->tv_sec) * 1000000 + (now.tv_usec - start->tv_usec);
    /* ponytail: keep every sample for exact percentiles (4 bytes each). Fine into the
     * millions; swap for an HdrHistogram if runs get large enough for the array to hurt. */
    if (w->lat_n == w->lat_cap) {
        w->lat_cap = w->lat_cap != 0 ? w->lat_cap * 2 : 1024;
        w->lat_us = h2o_mem_realloc(w->lat_us, w->lat_cap * sizeof(uint32_t));
    }
    w->lat_us[w->lat_n++] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* p in [0,100] over an ascending-sorted array */
static double pctl_ms(const uint32_t *sorted, size_t n, double p)
{
    if (n == 0)
        return 0;
    size_t idx = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx] / 1000.0;
}

/* Merge the per-worker counters and latency samples, print the run summary, and return total failed. */
unsigned print_summary(struct worker *workers, double elapsed)
{
    unsigned n_ok = 0, n_fail = 0, n_resumed = 0, conns = 0;
    uint64_t bytes = 0;
    size_t total_lat = 0;
    for (unsigned i = 0; i < conf.threads; ++i) {
        n_ok += workers[i].n_ok;
        n_fail += workers[i].n_fail;
        n_resumed += workers[i].n_resumed;
        conns += workers[i].conn_count;
        bytes += workers[i].body_bytes;
        total_lat += workers[i].lat_n;
    }
    uint32_t *lat = h2o_mem_alloc(sizeof(uint32_t) * (total_lat != 0 ? total_lat : 1));
    size_t off = 0;
    for (unsigned i = 0; i < conf.threads; ++i) {
        memcpy(lat + off, workers[i].lat_us, workers[i].lat_n * sizeof(uint32_t));
        off += workers[i].lat_n;
        free(workers[i].lat_us);
    }
    qsort(lat, total_lat, sizeof(uint32_t), cmp_u32);

    double mean = 0;
    for (size_t i = 0; i < total_lat; ++i)
        mean += lat[i];
    if (total_lat != 0)
        mean = mean / total_lat / 1000.0;

    fprintf(stderr, "\n");
    fprintf(stderr, "completed:   %u    failed: %u\n", n_ok, n_fail);
    /* in reconnect mode conn_count tallies the churned episodes; otherwise every pool opens exactly
     * one connection, so the intended total is the accurate figure */
    fprintf(stderr, "connections: %u    resumed (PSK/0-RTT): %u requests\n",
            conf.reconnect != 0 ? conns : conf.connections, n_resumed);
    fprintf(stderr, "duration:    %.3f s\n", elapsed);
    fprintf(stderr, "throughput:  %.0f req/s    %.2f MB/s\n", elapsed > 0 ? n_ok / elapsed : 0,
            elapsed > 0 ? bytes / elapsed / (1024 * 1024) : 0);
    fprintf(stderr, "latency ms:  min %.2f  mean %.2f  p50 %.2f  p90 %.2f  p99 %.2f  max %.2f\n", pctl_ms(lat, total_lat, 0),
            mean, pctl_ms(lat, total_lat, 50), pctl_ms(lat, total_lat, 90), pctl_ms(lat, total_lat, 99),
            pctl_ms(lat, total_lat, 100));

    free(lat);
    return n_fail;
}

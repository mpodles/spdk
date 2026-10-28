/*   SPDX-License-Identifier: BSD-3-Clause
 *   Open-loop traffic for spdk_nvme_perf: a C interface to korba's traffic
 *   engine (subprojects/korba/src/traffic_engine.hpp). One engine per perf
 *   core; only that core touches it. Spec grammar: korba's TRAFFIC
 *   (docs/clients_and_traffic.md in the korba tree).
 */
#ifndef PERF_TRAFFIC_SHIM_H
#define PERF_TRAFFIC_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct perf_traffic;

/* Checks spec. *bursty: a bursty: spec (carries its own total load);
 * otherwise a fixed: spec, which needs a per-core rate. 0 on success, else
 * -1 with err filled. */
int perf_traffic_check(const char *spec, int *bursty, char *err, size_t errlen);

/* One core's engine over its n queues. global_ids[i]: queue i's global index
 * (seeds its generator, so its traffic does not depend on the core driving
 * it); total: queues over all cores. rate_per_s: the core's requests per
 * second, fixed: only. ticks_per_s / t0: the clock. NULL with err on error. */
struct perf_traffic *perf_traffic_create(const char *spec, const uint32_t *global_ids,
		uint32_t n, uint32_t total, uint64_t rate_per_s, uint64_t ticks_per_s,
		uint64_t t0, uint64_t seed, char *err, size_t errlen);

/* Arrivals due by now, in time order: sched[i] (ticks), queue[i] (0..n-1).
 * At most max; the rest stay due for the next call. */
uint32_t perf_traffic_poll(struct perf_traffic *t, uint64_t now, uint64_t *sched,
			   uint32_t *queue, uint32_t max);

/* Offered requests per second over all cores (bursty:), or this core's rate
 * (fixed:). */
double perf_traffic_offered(const struct perf_traffic *t);

/* bursty: the spec's Burst classes (1 for fixed:). */
uint32_t perf_traffic_classes(const struct perf_traffic *t);

/* bursty: the Burst class of queue i (0 for fixed:). */
uint32_t perf_traffic_queue_class(const struct perf_traffic *t, uint32_t i);

/* bursty: queue i's request rate per second while bursting (b = 1), while
 * quiet (b = 0), or on average over both states (b = 2). 0 for fixed:. */
double perf_traffic_queue_rate(const struct perf_traffic *t, uint32_t i, uint32_t b);

/* The spec in korba's canonical spelling. */
void perf_traffic_format(const struct perf_traffic *t, char *out, size_t len);

void perf_traffic_destroy(struct perf_traffic *t);

#ifdef __cplusplus
}
#endif

#endif

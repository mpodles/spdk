/*   SPDX-License-Identifier: BSD-3-Clause
 *   C interface to korba's traffic engine for spdk_nvme_perf (traffic_shim.h).
 */
#include "traffic_shim.h"

#include "traffic_engine.hpp"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

struct perf_traffic {
	TrafficSpec spec;
	TrafficEngine engine;
	std::vector<TrafficArrival> buf;
	double offered = 0;
};

static void
set_err(char *err, size_t len, const std::string &msg)
{
	if (err != nullptr && len > 0) {
		snprintf(err, len, "%s", msg.c_str());
	}
}

/* A command has no known Processing time, so rho: loads and the worker cap
 * (both defined from it in korba) are refused. */
static bool
check_spec(const char *spec, TrafficSpec &out, std::string &err)
{
	if (!traffic_spec_parse(spec, out, err)) {
		return false;
	}
	if (out.mode == TrafficMode::BURSTY) {
		if (out.load != TrafficLoad::RPS) {
			err = std::string(spec) + ": perf needs an rps: load (a command has no "
			      "known Processing time for rho:)";
			return false;
		}
		if (out.cap_worker) {
			err = std::string(spec) + ": the worker cap needs a Processing time; use none";
			return false;
		}
	}
	return true;
}

extern "C" int
perf_traffic_check(const char *spec, int *bursty, char *err, size_t errlen)
{
	TrafficSpec s;
	std::string e;
	if (!check_spec(spec, s, e)) {
		set_err(err, errlen, e);
		return -1;
	}
	if (bursty != nullptr) {
		*bursty = s.mode == TrafficMode::BURSTY;
	}
	return 0;
}

extern "C" struct perf_traffic *
perf_traffic_create(const char *spec, const uint32_t *global_ids, uint32_t n, uint32_t total,
		    uint64_t rate_per_s, uint64_t ticks_per_s, uint64_t t0, uint64_t seed,
		    char *err, size_t errlen)
{
	perf_traffic *t = new (std::nothrow) perf_traffic();
	if (t == nullptr) {
		set_err(err, errlen, "out of memory");
		return nullptr;
	}
	std::string e;
	bool ok = check_spec(spec, t->spec, e);
	if (ok && t->spec.mode == TrafficMode::BURSTY) {
		ok = t->engine.init_bursty(t->spec, global_ids, n, total,
					   (double)t->spec.load_value, 0.0, ticks_per_s, t0, seed, e);
		t->offered = t->engine.offered_per_s();
	} else if (ok) {
		ok = traffic_spec_fits(t->spec, n, e) &&
		     t->engine.init(t->spec, n, rate_per_s, ticks_per_s, t0, seed, e);
		t->offered = (double)rate_per_s;
	}
	if (!ok) {
		set_err(err, errlen, e);
		delete t;
		return nullptr;
	}
	t->buf.resize(256);
	return t;
}

extern "C" uint32_t
perf_traffic_poll(struct perf_traffic *t, uint64_t now, uint64_t *sched, uint32_t *queue,
		  uint32_t max)
{
	if (max > t->buf.size()) {
		max = (uint32_t)t->buf.size();
	}
	const uint32_t got = t->engine.poll(now, t->buf.data(), max);
	for (uint32_t i = 0; i < got; i++) {
		sched[i] = t->buf[i].t;
		queue[i] = t->buf[i].conn;
	}
	return got;
}

extern "C" double
perf_traffic_offered(const struct perf_traffic *t)
{
	return t->offered;
}

extern "C" void
perf_traffic_format(const struct perf_traffic *t, char *out, size_t len)
{
	snprintf(out, len, "%s", traffic_spec_format(t->spec).c_str());
}

extern "C" void
perf_traffic_destroy(struct perf_traffic *t)
{
	delete t;
}

/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * xrc_verify: data-integrity load for NVMe-oF/RDMA, written for the XRC work
 * (commands of one queue running on any target poll group), usable over RC too.
 *
 * Each I/O queue owns a disjoint LBA region cut into slots of one I/O size.
 * Every 512-byte unit written carries (magic, LBA, version, region seed) and a
 * pattern derived from them, so a read proves which write it returns. A slot
 * has either one write or any number of reads in flight, never both, so the
 * expected version of a read is the last completed write: a read that
 * follows a write of the same queue (and, with round-robin steering, lands on
 * another target core) must see it.
 *
 * Phases per queue: prefill (version 1 everywhere), timed random reads/writes
 * (-M read percent), final read-back of every slot. Any mismatch fails the
 * run. With --tolerate-errors, failed commands (e.g. namespace removed,
 * controller going away) are counted, a failed write leaves its slot holding
 * either the old or the new version, and the run only fails on mismatches.
 *
 * Extra checks: --abort-rate N sends N NVMe Aborts per second for random
 * outstanding commands; --fused-check sends one compare-and-write pair and
 * reports its status (expected: refused on XRC queues).
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk_internal/nvmf_rdma_xrc.h"

#define UNIT		512u
#define MAGIC		0x58524356u	/* "XRCV" */
#define MAX_WORKERS	64

struct unit_hdr {
	uint32_t	magic;
	uint32_t	version;
	uint64_t	lba;
	uint32_t	seed;
	uint32_t	rsvd;
};

enum slot_state {
	SLOT_IDLE,
	SLOT_READING,
	SLOT_WRITING,
};

struct slot {
	uint32_t	version;	/* last completed write */
	uint32_t	maybe;		/* != 0: a failed write may have landed this version */
	uint16_t	readers;
	uint8_t		state;
};

struct worker;

struct task {
	struct qpair_ctx	*q;
	void			*buf;
	uint32_t		slot;
	uint32_t		version;
	bool			is_write;
	bool			busy;
	uint64_t		submit_tsc;
};

struct qpair_ctx {
	struct worker		*w;
	struct spdk_nvme_qpair	*qpair;
	uint32_t		idx;
	uint64_t		region_lba;
	uint32_t		nslots;
	struct slot		*slots;
	struct task		*tasks;
	uint32_t		inflight;
	uint32_t		phase_next;	/* prefill / final: next slot */
	uint32_t		seed;
	uint64_t		rand;
	bool			connected;
};

struct worker {
	uint32_t			lcore;
	struct spdk_nvme_poll_group	*group;
	struct qpair_ctx		*q;
	uint32_t			nq;
	/* stats */
	uint64_t			reads, writes, read_bytes, write_bytes;
	uint64_t			mismatches, io_errors, unresolved;
	uint64_t			aborts_sent, aborts_done, aborts_hit;
	uint64_t			readbacks;
	bool				draining;
	uint64_t			lat_sum, lat_max;
	int				rc;
};

static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns *g_ns;
static struct spdk_nvme_transport_id g_trid;
static char g_hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
static uint32_t g_nsid = 1;
static uint32_t g_io_size = 4096;
static uint32_t g_qd = 32;
static uint32_t g_qpairs_per_core = 1;
static int g_read_pct = 70;
static int g_time = 10;
static uint64_t g_region_mb = 64;
static uint64_t g_region_offset_mb;
static bool g_xrc;
static int g_steering = SPDK_NVME_RDMA_XRC_STEER_HOME;
static bool g_tolerate;
static bool g_no_prefill;
static uint32_t g_abort_rate;
static bool g_fused_check;
static bool g_readback;
static int g_hold;
static int g_shm_id = -1;
static const char *g_core_mask = "0x1";
static uint32_t g_seed = 1;
static volatile bool g_stop;
/* Workers still in their I/O phases; worker 0 serves the admin queue until 0. */
static uint32_t g_busy_workers;
static struct worker g_workers[MAX_WORKERS];
static uint32_t g_nworkers;
static uint32_t g_sector;

enum phase {
	PHASE_PREFILL,
	PHASE_RANDOM,
	PHASE_FINAL,
	PHASE_DONE,
};

static uint64_t
xorshift64(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return x;
}

static void
fill_unit(uint8_t *u, uint64_t lba, uint32_t version, uint32_t seed)
{
	struct unit_hdr *h = (struct unit_hdr *)u;
	uint64_t s = (lba * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)version << 32) ^ seed ^ 1;
	uint64_t *p;
	uint32_t i;

	h->magic = MAGIC;
	h->version = version;
	h->lba = lba;
	h->seed = seed;
	h->rsvd = 0;
	p = (uint64_t *)(u + sizeof(*h));
	for (i = 0; i < (UNIT - sizeof(*h)) / 8; i++) {
		p[i] = xorshift64(&s);
	}
}

/* The version the unit holds if it is intact, 0 if it is not a valid unit of
 * this LBA and region. */
static uint32_t
check_unit(const uint8_t *u, uint64_t lba, uint32_t seed)
{
	const struct unit_hdr *h = (const struct unit_hdr *)u;
	uint8_t ref[UNIT];

	if (h->magic != MAGIC || h->lba != lba || h->seed != seed) {
		return 0;
	}
	fill_unit(ref, lba, h->version, seed);
	return memcmp(ref, u, UNIT) == 0 ? h->version : 0;
}

static void
fill_buf(struct qpair_ctx *q, void *buf, uint32_t slot, uint32_t version)
{
	uint64_t lba0 = q->region_lba * g_sector / UNIT + (uint64_t)slot * (g_io_size / UNIT);
	uint32_t i;

	for (i = 0; i < g_io_size / UNIT; i++) {
		fill_unit((uint8_t *)buf + i * UNIT, lba0 + i, version, q->seed);
	}
}

/* Returns true if the buffer is a consistent copy of an acceptable version. */
static bool
verify_buf(struct qpair_ctx *q, struct task *t)
{
	struct slot *s = &q->slots[t->slot];
	uint64_t lba0 = q->region_lba * g_sector / UNIT + (uint64_t)t->slot * (g_io_size / UNIT);
	uint32_t i, v, first = 0;

	for (i = 0; i < g_io_size / UNIT; i++) {
		v = check_unit((uint8_t *)t->buf + i * UNIT, lba0 + i, q->seed);
		if (i == 0) {
			first = v;
		}
		if (v == 0 || v != first) {
			const struct unit_hdr *h = (const struct unit_hdr *)((uint8_t *)t->buf + i * UNIT);

			fprintf(stderr, "MISMATCH lcore %u qpair %u slot %u unit %u: got version %u "
				"(first unit %u), expected %u%s%u; unit header magic 0x%08x version %u lba %" PRIu64
				" seed %u (want lba %" PRIu64 " seed %u)\n", q->w->lcore, q->idx, t->slot, i, v, first,
				s->version, s->maybe ? " or " : "", s->maybe, h->magic, h->version, h->lba, h->seed,
				lba0 + i, q->seed);
			return false;
		}
	}
	if (first == s->version) {
		s->maybe = 0;
		return true;
	}
	if (s->maybe != 0 && first == s->maybe) {
		/* The failed write did land. */
		s->version = s->maybe;
		s->maybe = 0;
		return true;
	}
	fprintf(stderr, "MISMATCH lcore %u qpair %u slot %u: read version %u, expected %u%s%u\n",
		q->w->lcore, q->idx, t->slot, first, s->version, s->maybe ? " or " : "", s->maybe);
	return false;
}

static enum phase g_phase_of_worker[MAX_WORKERS];

static int issue(struct qpair_ctx *q, struct task *t, uint32_t slot, bool is_write);


static void
io_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct task *t = arg;
	struct qpair_ctx *q = t->q;
	struct worker *w = q->w;
	struct slot *s = &q->slots[t->slot];
	uint64_t lat = spdk_get_ticks() - t->submit_tsc;

	t->busy = false;
	q->inflight--;
	w->lat_sum += lat;
	if (lat > w->lat_max) {
		w->lat_max = lat;
	}

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		w->io_errors++;
		if (!g_tolerate || w->io_errors <= 10) {
			fprintf(stderr, "I/O error lcore %u qpair %u slot %u %s: sct 0x%x sc 0x%x\n", w->lcore,
				q->idx, t->slot, t->is_write ? "write" : "read", cpl->status.sct, cpl->status.sc);
		}
		if (t->is_write) {
			s->maybe = t->version;
			s->state = SLOT_IDLE;
		} else if (--s->readers == 0) {
			s->state = SLOT_IDLE;
		}
		return;
	}

	if (t->is_write) {
		w->writes++;
		w->write_bytes += g_io_size;
		s->version = t->version;
		s->maybe = 0;
		s->state = SLOT_IDLE;
		if (g_readback && q->connected && g_phase_of_worker[w - g_workers] == PHASE_RANDOM) {
			/* Read the slot back right away: with round-robin steering the
			 * read runs on another target core than the write did. */
			if (issue(q, t, t->slot, false) == 0) {
				w->readbacks++;
			}
		}
	} else {
		w->reads++;
		w->read_bytes += g_io_size;
		if (!verify_buf(q, t)) {
			w->mismatches++;
		}
		if (--s->readers == 0) {
			s->state = SLOT_IDLE;
		}
	}
}

static int
issue(struct qpair_ctx *q, struct task *t, uint32_t slot, bool is_write)
{
	struct slot *s = &q->slots[slot];
	uint64_t lba = q->region_lba + (uint64_t)slot * (g_io_size / g_sector);
	int rc;

	t->slot = slot;
	t->is_write = is_write;
	t->submit_tsc = spdk_get_ticks();
	if (is_write) {
		t->version = spdk_max(s->version, s->maybe) + 1;
		fill_buf(q, t->buf, slot, t->version);
		rc = spdk_nvme_ns_cmd_write(g_ns, q->qpair, t->buf, lba, g_io_size / g_sector, io_done, t, 0);
		if (rc == 0) {
			s->state = SLOT_WRITING;
		}
	} else {
		/* A read that moves no data must not pass on stale contents. */
		memset(t->buf, 0xA5, g_io_size);
		rc = spdk_nvme_ns_cmd_read(g_ns, q->qpair, t->buf, lba, g_io_size / g_sector, io_done, t, 0);
		if (rc == 0) {
			s->state = SLOT_READING;
			s->readers++;
		}
	}
	if (rc == 0) {
		t->busy = true;
		q->inflight++;
	}
	return rc;
}

/* Fill free tasks of a queue according to its phase. */
static void
pump(struct qpair_ctx *q, enum phase phase)
{
	struct task *t;
	uint32_t i, slot, tries;
	bool is_write;

	if (!q->connected) {
		return;
	}
	for (i = 0; i < g_qd; i++) {
		t = &q->tasks[i];
		if (t->busy) {
			continue;
		}
		if (phase == PHASE_PREFILL || phase == PHASE_FINAL) {
			if (q->phase_next >= q->nslots) {
				return;
			}
			slot = q->phase_next;
			if (q->slots[slot].state != SLOT_IDLE) {
				return;
			}
			q->phase_next++;
			if (issue(q, t, slot, phase == PHASE_PREFILL)) {
				q->phase_next--;
				return;
			}
			continue;
		}
		if (phase != PHASE_RANDOM || g_stop) {
			return;
		}
		is_write = (int)(xorshift64(&q->rand) % 100) >= g_read_pct;
		for (tries = 0; tries < 8; tries++) {
			slot = xorshift64(&q->rand) % q->nslots;
			if (is_write ? q->slots[slot].state == SLOT_IDLE :
			    q->slots[slot].state != SLOT_WRITING) {
				break;
			}
		}
		if (tries == 8) {
			return;
		}
		if (issue(q, t, slot, is_write)) {
			return;
		}
	}
}

static void
disconnected_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{
	struct worker *w = ctx;
	uint32_t i;

	for (i = 0; i < w->nq; i++) {
		if (w->q[i].qpair == qpair && w->q[i].connected) {
			w->q[i].connected = false;
			fprintf(stderr, "lcore %u qpair %u disconnected (reason %d)\n", w->lcore, i,
				spdk_nvme_qpair_get_failure_reason(qpair));
			if (!g_tolerate) {
				w->rc = -1;
			}
		}
	}
}

static bool
phase_quiet(struct worker *w, enum phase phase)
{
	uint32_t i;

	for (i = 0; i < w->nq; i++) {
		struct qpair_ctx *q = &w->q[i];

		if (q->inflight != 0) {
			return false;
		}
		if ((phase == PHASE_PREFILL || phase == PHASE_FINAL) && q->connected &&
		    q->phase_next < q->nslots) {
			return false;
		}
	}
	return true;
}

struct abort_ctx {
	struct worker	*w;
};

static void
abort_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct worker *w = arg;

	w->aborts_done++;
	if (!spdk_nvme_cpl_is_error(cpl) && (cpl->cdw0 & 1u) == 0) {
		w->aborts_hit++;
	}
}

static void
fused_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	int *state = arg;

	printf("FUSED %s: sct 0x%x sc 0x%x dnr %u\n", *state == 0 ? "compare (FIRST)" : "write (SECOND)",
	       cpl->status.sct, cpl->status.sc, cpl->status.dnr);
	(*state)++;
}

static void
run_fused_check(struct worker *w)
{
	struct qpair_ctx *q = &w->q[0];
	void *buf = spdk_zmalloc(g_sector, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	void *buf2 = spdk_zmalloc(g_sector, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	int done = 0, rc;

	rc = spdk_nvme_ns_cmd_compare(g_ns, q->qpair, buf, q->region_lba, 1, fused_done, &done,
				      SPDK_NVME_IO_FLAGS_FUSE_FIRST);
	if (rc == 0) {
		rc = spdk_nvme_ns_cmd_write(g_ns, q->qpair, buf2, q->region_lba, 1, fused_done, &done,
					    SPDK_NVME_IO_FLAGS_FUSE_SECOND);
	}
	if (rc) {
		printf("FUSED submit failed: %d (%s)\n", rc, spdk_strerror(-rc));
		w->rc = -1;
	} else {
		uint64_t deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();

		while (done < 2 && spdk_get_ticks() < deadline) {
			spdk_nvme_poll_group_process_completions(w->group, 0, disconnected_cb);
			spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
		}
		if (done < 2) {
			printf("FUSED timeout: %d of 2 completions\n", done);
			w->rc = -1;
		}
	}
	spdk_free(buf);
	spdk_free(buf2);
}

static int
worker_fn(void *arg)
{
	struct worker *w = arg;
	struct spdk_nvme_io_qpair_opts opts;
	uint64_t tsc_hz = spdk_get_ticks_hz(), end = 0, next_abort = 0, next_admin = 0, now;
	enum phase phase;
	uint32_t i, j;

	w->group = spdk_nvme_poll_group_create(w, NULL);
	if (w->group == NULL) {
		w->rc = -1;
		__atomic_sub_fetch(&g_busy_workers, 1, __ATOMIC_SEQ_CST);
		return -1;
	}
	spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = spdk_max(opts.io_queue_size, g_qd + 1);
	opts.io_queue_requests = spdk_max(opts.io_queue_requests, g_qd * 4);
	opts.delay_cmd_submit = true;
	opts.create_only = true;

	for (i = 0; i < w->nq; i++) {
		struct qpair_ctx *q = &w->q[i];

		q->qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &opts, sizeof(opts));
		if (q->qpair == NULL || spdk_nvme_poll_group_add(w->group, q->qpair) ||
		    spdk_nvme_ctrlr_connect_io_qpair(g_ctrlr, q->qpair)) {
			fprintf(stderr, "lcore %u: cannot create qpair %u\n", w->lcore, i);
			w->rc = -1;
			__atomic_sub_fetch(&g_busy_workers, 1, __ATOMIC_SEQ_CST);
			return -1;
		}
		q->connected = true;
		for (j = 0; j < g_qd; j++) {
			q->tasks[j].q = q;
			q->tasks[j].buf = spdk_zmalloc(g_io_size, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
			if (q->tasks[j].buf == NULL) {
				w->rc = -1;
			__atomic_sub_fetch(&g_busy_workers, 1, __ATOMIC_SEQ_CST);
				return -1;
			}
		}
	}
	while (spdk_nvme_poll_group_all_connected(w->group) == -EAGAIN) {
		spdk_nvme_poll_group_process_completions(w->group, 0, disconnected_cb);
	}

	if (g_fused_check && w == &g_workers[0]) {
		run_fused_check(w);
	}

	phase = g_no_prefill ? PHASE_RANDOM : PHASE_PREFILL;
	if (phase == PHASE_RANDOM) {
		end = spdk_get_ticks() + g_time * tsc_hz;
	}
	while (phase != PHASE_DONE) {
		/* Past the timed phase's end nothing new is issued (PHASE_DONE). */
		g_phase_of_worker[w - g_workers] = (phase == PHASE_RANDOM && w->draining) ? PHASE_DONE : phase;
		for (i = 0; i < w->nq; i++) {
			pump(&w->q[i], g_phase_of_worker[w - g_workers]);
		}
		spdk_nvme_poll_group_process_completions(w->group, 0, disconnected_cb);
		now = spdk_get_ticks();

		if (g_abort_rate && phase == PHASE_RANDOM && now >= next_abort) {
			struct qpair_ctx *q = &w->q[xorshift64(&w->q[0].rand) % w->nq];
			struct task *t = &q->tasks[xorshift64(&w->q[0].rand) % g_qd];

			next_abort = now + tsc_hz / g_abort_rate;
			if (t->busy && q->connected &&
			    spdk_nvme_ctrlr_cmd_abort_ext(g_ctrlr, q->qpair, t, abort_done, w) == 0) {
				w->aborts_sent++;
			}
		}
		/* Worker 0 runs the admin queue (keep-alive, abort completions). */
		if (w == &g_workers[0] && (g_abort_rate || now >= next_admin)) {
			spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
			next_admin = now + tsc_hz / 100;
		}

		switch (phase) {
		case PHASE_PREFILL:
			if (phase_quiet(w, phase)) {
				phase = PHASE_RANDOM;
				end = now + g_time * tsc_hz;
			}
			break;
		case PHASE_RANDOM:
			if (now >= end || g_stop) {
				/* Stop issuing, wait for the queues to drain. */
				w->draining = true;
				if (phase_quiet(w, phase)) {
					for (i = 0; i < w->nq; i++) {
						w->q[i].phase_next = 0;
					}
					phase = PHASE_FINAL;
				}
			}
			break;
		case PHASE_FINAL:
			if (phase_quiet(w, phase)) {
				phase = PHASE_DONE;
			}
			break;
		default:
			break;
		}
	}
	while (g_abort_rate && w->aborts_done < w->aborts_sent) {
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	}
	__atomic_sub_fetch(&g_busy_workers, 1, __ATOMIC_SEQ_CST);
	/* Keep-alive and CM events (disconnects of other workers' queues) need
	 * the admin queue polled until every worker is done. */
	while (w == &g_workers[0] && __atomic_load_n(&g_busy_workers, __ATOMIC_SEQ_CST) > 0) {
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
		spdk_nvme_poll_group_process_completions(w->group, 0, disconnected_cb);
	}
	if (g_hold > 0) {
		/* Queues stay connected and idle, for target-side drain checks. */
		printf("HOLD lcore %u: %d s idle with queues connected\n", w->lcore, g_hold);
		fflush(stdout);
		end = spdk_get_ticks() + g_hold * tsc_hz;
		while (spdk_get_ticks() < end && !g_stop) {
			spdk_nvme_poll_group_process_completions(w->group, 0, disconnected_cb);
			if (w == &g_workers[0]) {
				spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
			}
		}
	}

	for (i = 0; i < w->nq; i++) {
		for (j = 0; j < w->q[i].nslots; j++) {
			if (w->q[i].slots[j].maybe != 0) {
				w->unresolved++;
			}
		}
		spdk_nvme_ctrlr_free_io_qpair(w->q[i].qpair);
	}
	spdk_nvme_poll_group_destroy(w->group);
	return 0;
}

static void
usage(const char *p)
{
	printf("%s -r 'trtype:RDMA adrfam:IPv4 traddr:A trsvcid:4420 subnqn:N' [options]\n", p);
	printf(" -c MASK      worker cores (default 0x1)\n");
	printf(" -P N         I/O queues per core (default 1)\n");
	printf(" -q N         queue depth per queue (default 32)\n");
	printf(" -o BYTES     I/O size, multiple of 512 (default 4096)\n");
	printf(" -M PCT       read percent in the timed phase (default 70)\n");
	printf(" -t SEC       timed phase length (default 10)\n");
	printf(" -n NSID      namespace (default 1)\n");
	printf(" -H NQN       host NQN\n");
	printf(" -i ID        shared memory id\n");
	printf(" -R MB        region per queue in MiB (default 64)\n");
	printf(" -O MB        offset of this process's regions in MiB (default 0)\n");
	printf(" -S SEED      region seed (default 1)\n");
	printf(" -x           I/O queues over XRC\n");
	printf(" -s POLICY    XRC steering: home, rr, random (default home)\n");
	printf(" -T           tolerate I/O errors and disconnects (only mismatches fail)\n");
	printf(" -N           no prefill (regions already hold this seed's version 1)\n");
	printf(" -A RATE      NVMe Aborts per second in the timed phase\n");
	printf(" -F           send one fused compare-and-write first and report its status\n");
	printf(" -B           read every slot back right after writing it (timed phase)\n");
	printf(" -h SEC       hold the queues connected and idle for SEC s at the end\n");
}

static bool
attach_probe_cb(void *ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
sigint(int sig)
{
	g_stop = true;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts env;
	struct spdk_nvme_ctrlr_opts copts;
	struct spdk_nvme_transport_opts topts;
	struct spdk_nvme_detach_ctx *detach = NULL;
	uint32_t lcore, i, j, total_q = 0, wi;
	uint64_t ns_bytes, region_lbas, next_lba;
	uint64_t reads = 0, writes = 0, rb = 0, wb = 0, mism = 0, ioerr = 0, unres = 0, lat = 0, latmax = 0;
	uint64_t as = 0, ah = 0, rbk = 0;
	int op, rc = 0;

	(void)attach_probe_cb;
	memset(&g_trid, 0, sizeof(g_trid));
	while ((op = getopt(argc, argv, "r:c:P:q:o:M:t:n:H:i:R:O:S:xs:TNA:FBh:")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg)) {
				fprintf(stderr, "bad -r\n");
				return 1;
			}
			break;
		case 'c':
			g_core_mask = optarg;
			break;
		case 'P':
			g_qpairs_per_core = atoi(optarg);
			break;
		case 'q':
			g_qd = atoi(optarg);
			break;
		case 'o':
			g_io_size = atoi(optarg);
			break;
		case 'M':
			g_read_pct = atoi(optarg);
			break;
		case 't':
			g_time = atoi(optarg);
			break;
		case 'n':
			g_nsid = atoi(optarg);
			break;
		case 'H':
			snprintf(g_hostnqn, sizeof(g_hostnqn), "%s", optarg);
			break;
		case 'i':
			g_shm_id = atoi(optarg);
			break;
		case 'R':
			g_region_mb = strtoull(optarg, NULL, 10);
			break;
		case 'O':
			g_region_offset_mb = strtoull(optarg, NULL, 10);
			break;
		case 'S':
			g_seed = strtoul(optarg, NULL, 10);
			break;
		case 'x':
			g_xrc = true;
			break;
		case 's':
			if (strcmp(optarg, "home") == 0) {
				g_steering = SPDK_NVME_RDMA_XRC_STEER_HOME;
			} else if (strcmp(optarg, "rr") == 0) {
				g_steering = SPDK_NVME_RDMA_XRC_STEER_RR;
			} else if (strcmp(optarg, "random") == 0) {
				g_steering = SPDK_NVME_RDMA_XRC_STEER_RANDOM;
			} else {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'T':
			g_tolerate = true;
			break;
		case 'N':
			g_no_prefill = true;
			break;
		case 'A':
			g_abort_rate = atoi(optarg);
			break;
		case 'F':
			g_fused_check = true;
			break;
		case 'B':
			g_readback = true;
			break;
		case 'h':
			g_hold = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (g_trid.trtype == 0 || g_io_size == 0 || g_io_size % UNIT || g_qd == 0 ||
	    g_read_pct < 0 || g_read_pct > 100) {
		usage(argv[0]);
		return 1;
	}

	spdk_env_opts_init(&env);
	env.opts_size = sizeof(env);
	env.name = "xrc_verify";
	env.core_mask = g_core_mask;
	env.shm_id = g_shm_id;
	if (spdk_env_init(&env) < 0) {
		fprintf(stderr, "spdk_env_init failed\n");
		return 1;
	}
	signal(SIGINT, sigint);
	signal(SIGTERM, sigint);
	/* XRC notices (per-worker command counts, flushed buffers) go to stderr. */
	spdk_log_set_print_level(SPDK_LOG_NOTICE);

	if (g_xrc) {
		spdk_nvme_transport_get_opts(&topts, sizeof(topts));
		topts.rdma_xrc = true;
		topts.rdma_xrc_steering = g_steering;
		if (spdk_nvme_transport_set_opts(&topts, sizeof(topts))) {
			fprintf(stderr, "cannot set transport opts\n");
			return 1;
		}
	}

	SPDK_ENV_FOREACH_CORE(lcore) {
		if (g_nworkers == MAX_WORKERS) {
			break;
		}
		g_workers[g_nworkers].lcore = lcore;
		g_workers[g_nworkers].nq = g_qpairs_per_core;
		g_nworkers++;
	}
	total_q = g_nworkers * g_qpairs_per_core;
	if ((g_abort_rate || g_fused_check) && g_nworkers != 1) {
		fprintf(stderr, "-A and -F need a single worker core\n");
		return 1;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&copts, sizeof(copts));
	copts.num_io_queues = total_q + 1;
	copts.keep_alive_timeout_ms = 10000;
	if (g_hostnqn[0]) {
		snprintf(copts.hostnqn, sizeof(copts.hostnqn), "%s", g_hostnqn);
	}
	g_ctrlr = spdk_nvme_connect(&g_trid, &copts, sizeof(copts));
	if (g_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect failed\n");
		return 1;
	}
	g_ns = spdk_nvme_ctrlr_get_ns(g_ctrlr, g_nsid);
	if (g_ns == NULL || !spdk_nvme_ns_is_active(g_ns)) {
		fprintf(stderr, "namespace %u not active\n", g_nsid);
		return 1;
	}
	g_sector = spdk_nvme_ns_get_sector_size(g_ns);
	if (g_io_size % g_sector || g_sector % UNIT) {
		fprintf(stderr, "I/O size %u not a multiple of the sector size %u\n", g_io_size, g_sector);
		return 1;
	}
	ns_bytes = spdk_nvme_ns_get_size(g_ns);
	region_lbas = g_region_mb * 1024 * 1024 / g_sector;
	region_lbas -= region_lbas % (g_io_size / g_sector);
	next_lba = g_region_offset_mb * 1024 * 1024 / g_sector;
	if ((next_lba + region_lbas * total_q) * g_sector > ns_bytes || region_lbas == 0) {
		fprintf(stderr, "regions (%u x %" PRIu64 " MiB at %" PRIu64 " MiB) exceed the namespace (%" PRIu64
			" MiB)\n", total_q, g_region_mb, g_region_offset_mb, ns_bytes >> 20);
		return 1;
	}

	for (wi = 0; wi < g_nworkers; wi++) {
		struct worker *w = &g_workers[wi];

		w->q = calloc(w->nq, sizeof(*w->q));
		for (i = 0; i < w->nq; i++) {
			struct qpair_ctx *q = &w->q[i];

			q->w = w;
			q->idx = wi * g_qpairs_per_core + i;
			q->region_lba = next_lba;
			next_lba += region_lbas;
			q->nslots = region_lbas / (g_io_size / g_sector);
			q->slots = calloc(q->nslots, sizeof(*q->slots));
			q->tasks = calloc(g_qd, sizeof(*q->tasks));
			q->seed = g_seed * 1000003u + q->idx;
			q->rand = ((uint64_t)q->seed << 32) | 0x1234567u;
			if (g_no_prefill) {
				for (j = 0; j < q->nslots; j++) {
					q->slots[j].version = 1;
				}
			}
		}
	}

	printf("xrc_verify: %s %s, %u workers x %u queues, depth %u, %u B, %d%% reads, %d s, xrc %d "
	       "steering %d, %u slots per queue\n", g_trid.traddr, g_trid.subnqn, g_nworkers,
	       g_qpairs_per_core, g_qd, g_io_size, g_read_pct, g_time, g_xrc, g_steering,
	       g_workers[0].q[0].nslots);

	g_busy_workers = g_nworkers;
	for (wi = 1; wi < g_nworkers; wi++) {
		spdk_env_thread_launch_pinned(g_workers[wi].lcore, worker_fn, &g_workers[wi]);
	}
	worker_fn(&g_workers[0]);
	spdk_env_thread_wait_all();

	for (wi = 0; wi < g_nworkers; wi++) {
		struct worker *w = &g_workers[wi];

		reads += w->reads;
		writes += w->writes;
		rb += w->read_bytes;
		wb += w->write_bytes;
		mism += w->mismatches;
		ioerr += w->io_errors;
		unres += w->unresolved;
		lat += w->lat_sum;
		latmax = spdk_max(latmax, w->lat_max);
		as += w->aborts_sent;
		ah += w->aborts_hit;
		rbk += w->readbacks;
		if (w->rc) {
			rc = 1;
		}
	}
	printf("RESULT reads %" PRIu64 " writes %" PRIu64 " read_MiB %" PRIu64 " write_MiB %" PRIu64
	       " mismatches %" PRIu64 " io_errors %" PRIu64 " unresolved_slots %" PRIu64
	       " avg_lat_us %.1f max_lat_us %.1f aborts_sent %" PRIu64 " aborts_hit %" PRIu64 " readbacks %" PRIu64 "\n",
	       reads, writes, rb >> 20, wb >> 20, mism, ioerr, unres,
	       (reads + writes) ? (double)lat * 1e6 / spdk_get_ticks_hz() / (double)(reads + writes) : 0.0,
	       (double)latmax * 1e6 / spdk_get_ticks_hz(), as, ah, rbk);
	if (mism != 0 || (!g_tolerate && ioerr != 0)) {
		rc = 1;
	}
	printf("VERDICT %s\n", rc ? "FAIL" : "PASS");

	spdk_nvme_detach_async(g_ctrlr, &detach);
	if (detach) {
		spdk_nvme_detach_poll(detach);
	}
	spdk_env_fini();
	return rc;
}

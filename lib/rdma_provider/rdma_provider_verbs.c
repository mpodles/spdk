/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <rdma/rdma_cma.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk_internal/rdma_provider.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk/log.h"
#include "spdk/env.h"

struct spdk_rdma_provider_qp *
spdk_rdma_provider_qp_create(struct rdma_cm_id *cm_id,
			     struct spdk_rdma_provider_qp_init_attr *qp_attr)
{
	struct spdk_rdma_provider_qp *spdk_rdma_qp;
	int rc;
	struct ibv_qp_init_attr attr = {
		.qp_context = qp_attr->qp_context,
		.send_cq = qp_attr->cq->cq,
		.recv_cq = qp_attr->cq->cq,
		.cap = qp_attr->cap,
		.qp_type = IBV_QPT_RC
	};

	attr.srq = qp_attr->srq ? qp_attr->srq->srq : NULL;
	if (qp_attr->domain_transfer) {
		SPDK_ERRLOG("verbs provider doesn't support memory domain transfer functionality");
		return NULL;
	}

	spdk_rdma_qp = calloc(1, sizeof(*spdk_rdma_qp));
	if (!spdk_rdma_qp) {
		SPDK_ERRLOG("qp memory allocation failed\n");
		return NULL;
	}

	if (qp_attr->stats) {
		spdk_rdma_qp->stats = qp_attr->stats;
		spdk_rdma_qp->shared_stats = true;
	} else {
		spdk_rdma_qp->stats = calloc(1, sizeof(*spdk_rdma_qp->stats));
		if (!spdk_rdma_qp->stats) {
			SPDK_ERRLOG("qp statistics memory allocation failed\n");
			free(spdk_rdma_qp);
			return NULL;
		}
	}

	rc = rdma_create_qp(cm_id, qp_attr->pd, &attr);
	if (rc) {
		SPDK_ERRLOG("Failed to create qp, rc %d, errno %s (%d)\n", rc, spdk_strerror(errno), errno);
		free(spdk_rdma_qp);
		return NULL;
	}
	spdk_rdma_qp->qp = cm_id->qp;
	spdk_rdma_qp->cm_id = cm_id;
	spdk_rdma_qp->domain = spdk_rdma_utils_get_memory_domain(qp_attr->pd, SPDK_DMA_DEVICE_TYPE_RDMA);
	if (!spdk_rdma_qp->domain) {
		spdk_rdma_provider_qp_destroy(spdk_rdma_qp);
		return NULL;
	}

	qp_attr->cap = attr.cap;

	return spdk_rdma_qp;
}

int
spdk_rdma_provider_qp_accept(struct spdk_rdma_provider_qp *spdk_rdma_qp,
			     struct rdma_conn_param *conn_param)
{
	assert(spdk_rdma_qp != NULL);
	assert(spdk_rdma_qp->cm_id != NULL);

	return rdma_accept(spdk_rdma_qp->cm_id, conn_param);
}

int
spdk_rdma_provider_qp_complete_connect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	/* Nothing to be done for Verbs */
	return 0;
}

void
spdk_rdma_provider_qp_destroy(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	assert(spdk_rdma_qp != NULL);

	if (spdk_rdma_qp->send_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (spdk_rdma_qp->qp) {
		rdma_destroy_qp(spdk_rdma_qp->cm_id);
	}

	if (!spdk_rdma_qp->shared_stats) {
		free(spdk_rdma_qp->stats);
	}
	if (spdk_rdma_qp->domain) {
		spdk_rdma_utils_put_memory_domain(spdk_rdma_qp->domain);
	}

	free(spdk_rdma_qp);
}

int
spdk_rdma_provider_qp_disconnect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	int rc = 0;

	assert(spdk_rdma_qp != NULL);

	if (spdk_rdma_qp->cm_id) {
		rc = rdma_disconnect(spdk_rdma_qp->cm_id);
		if (rc) {
			if (errno == EINVAL && spdk_rdma_qp->qp->context->device->transport_type == IBV_TRANSPORT_IWARP) {
				/* rdma_disconnect may return an error and set errno to EINVAL in case of iWARP.
				 * This behaviour is expected since iWARP handles disconnect event other than IB and
				 * qpair is already in error state when we call rdma_disconnect */
				return 0;
			}
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}

	return rc;
}

bool
spdk_rdma_provider_qp_queue_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr *first)
{
	struct ibv_send_wr *last;

	assert(spdk_rdma_qp);
	assert(first);

	spdk_rdma_qp->stats->send.num_submitted_wrs++;
	last = first;
	while (last->next != NULL) {
		last = last->next;
		spdk_rdma_qp->stats->send.num_submitted_wrs++;
	}

	if (spdk_rdma_qp->send_wrs.first == NULL) {
		spdk_rdma_qp->send_wrs.first = first;
		spdk_rdma_qp->send_wrs.last = last;
		return true;
	} else {
		spdk_rdma_qp->send_wrs.last->next = first;
		spdk_rdma_qp->send_wrs.last = last;
		return false;
	}
}

int
spdk_rdma_provider_qp_flush_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr **bad_wr)
{
	int rc;

	assert(spdk_rdma_qp);
	assert(bad_wr);

	if (spdk_unlikely(!spdk_rdma_qp->send_wrs.first)) {
		return 0;
	}

	rc = ibv_post_send(spdk_rdma_qp->qp, spdk_rdma_qp->send_wrs.first, bad_wr);

	spdk_rdma_qp->send_wrs.first = NULL;
	spdk_rdma_qp->stats->send.doorbell_updates++;

	return rc;
}

struct spdk_rdma_provider_srq *
spdk_rdma_provider_srq_create(struct spdk_rdma_provider_srq_init_attr *init_attr)
{
	assert(init_attr);
	assert(init_attr->pd);

	struct spdk_rdma_provider_srq *rdma_srq = calloc(1, sizeof(*rdma_srq));

	if (!rdma_srq) {
		SPDK_ERRLOG("Can't allocate memory for SRQ handle\n");
		return NULL;
	}

	if (init_attr->stats) {
		rdma_srq->stats = init_attr->stats;
		rdma_srq->shared_stats = true;
	} else {
		rdma_srq->stats = calloc(1, sizeof(*rdma_srq->stats));
		if (!rdma_srq->stats) {
			SPDK_ERRLOG("SRQ statistics memory allocation failed");
			free(rdma_srq);
			return NULL;
		}
	}

	rdma_srq->srq = ibv_create_srq(init_attr->pd, &init_attr->srq_init_attr);
	if (!rdma_srq->srq) {
		if (!init_attr->stats) {
			free(rdma_srq->stats);
		}
		SPDK_ERRLOG("Unable to create SRQ, errno %d (%s)\n", errno, spdk_strerror(errno));
		free(rdma_srq);
		return NULL;
	}

	return rdma_srq;
}

int
spdk_rdma_provider_srq_destroy(struct spdk_rdma_provider_srq *rdma_srq)
{
	int rc;

	if (!rdma_srq) {
		return 0;
	}

	assert(rdma_srq->srq);

	if (rdma_srq->recv_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying RDMA SRQ with queued recv WRs\n");
	}

	rc = ibv_destroy_srq(rdma_srq->srq);
	if (rc) {
		SPDK_ERRLOG("SRQ destroy failed with %d\n", rc);
	}

	if (!rdma_srq->shared_stats) {
		free(rdma_srq->stats);
	}

	free(rdma_srq);

	return rc;
}

static inline bool
rdma_queue_recv_wrs(struct spdk_rdma_provider_recv_wr_list *recv_wrs, struct ibv_recv_wr *first,
		    struct spdk_rdma_provider_wr_stats *recv_stats)
{
	struct ibv_recv_wr *last;

	recv_stats->num_submitted_wrs++;
	last = first;
	while (last->next != NULL) {
		last = last->next;
		recv_stats->num_submitted_wrs++;
	}

	if (recv_wrs->first == NULL) {
		recv_wrs->first = first;
		recv_wrs->last = last;
		return true;
	} else {
		recv_wrs->last->next = first;
		recv_wrs->last = last;
		return false;
	}
}

bool
spdk_rdma_provider_srq_queue_recv_wrs(struct spdk_rdma_provider_srq *rdma_srq,
				      struct ibv_recv_wr *first)
{
	assert(rdma_srq);
	assert(first);

	return rdma_queue_recv_wrs(&rdma_srq->recv_wrs, first, rdma_srq->stats);
}

int
spdk_rdma_provider_srq_flush_recv_wrs(struct spdk_rdma_provider_srq *rdma_srq,
				      struct ibv_recv_wr **bad_wr)
{
	int rc;

	if (spdk_unlikely(rdma_srq->recv_wrs.first == NULL)) {
		return 0;
	}

	rc = ibv_post_srq_recv(rdma_srq->srq, rdma_srq->recv_wrs.first, bad_wr);

	rdma_srq->recv_wrs.first = NULL;
	rdma_srq->stats->doorbell_updates++;

	return rc;
}

bool
spdk_rdma_provider_qp_queue_recv_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_recv_wr *first)
{
	assert(spdk_rdma_qp);
	assert(first);

	return rdma_queue_recv_wrs(&spdk_rdma_qp->recv_wrs, first, &spdk_rdma_qp->stats->recv);
}

int
spdk_rdma_provider_qp_flush_recv_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_recv_wr **bad_wr)
{
	int rc;

	if (spdk_unlikely(spdk_rdma_qp->recv_wrs.first == NULL)) {
		return 0;
	}

	rc = ibv_post_recv(spdk_rdma_qp->qp, spdk_rdma_qp->recv_wrs.first, bad_wr);

	spdk_rdma_qp->recv_wrs.first = NULL;
	spdk_rdma_qp->stats->recv.doorbell_updates++;

	return rc;
}

bool
spdk_rdma_provider_accel_sequence_supported(void)
{
	return false;
}

/* A CQ with NIC completion timestamps (SPDK_RDMA_CQ_TS=1): the NIC clock is mapped to
 * spdk_get_ticks() by sampling both clocks every 20 ms. */
struct verbs_cq {
	struct spdk_rdma_provider_cq	base;
	struct ibv_cq_ex		*ex;
	struct ibv_context		*ctx;
	double				ticks_per_cycle;
	double				offset;
	uint64_t			next_sync;
};

static __thread uint64_t g_cq_ts[SPDK_RDMA_PROVIDER_CQ_TS_MAX];
static __thread bool g_cq_ts_valid;

const uint64_t *
spdk_rdma_provider_cq_poll_ts(void)
{
	return g_cq_ts_valid ? g_cq_ts : NULL;
}

static void
verbs_cq_sync_clock(struct verbs_cq *vcq, uint64_t now)
{
	struct ibv_values_ex values = { .comp_mask = IBV_VALUES_MASK_RAW_CLOCK };
	uint64_t t0, t1, raw;

	t0 = spdk_get_ticks();
	if (ibv_query_rt_values_ex(vcq->ctx, &values) != 0) {
		return;
	}
	t1 = spdk_get_ticks();
	raw = (uint64_t)values.raw_clock.tv_sec * 1000000000ULL + values.raw_clock.tv_nsec;
	vcq->offset = (double)(t0 + t1) / 2 - (double)raw * vcq->ticks_per_cycle;
	vcq->next_sync = now + spdk_get_ticks_hz() / 50;
}

struct spdk_rdma_provider_cq *
spdk_rdma_provider_cq_create(struct spdk_rdma_provider_cq_init_attr *cq_attr)
{
	struct verbs_cq *vcq;
	const char *ts = getenv("SPDK_RDMA_CQ_TS");

	vcq = calloc(1, sizeof(*vcq));
	if (!vcq) {
		SPDK_ERRLOG("CQ memory allocation failed\n");
		return NULL;
	}

	if (ts != NULL && strcmp(ts, "1") == 0) {
		struct ibv_cq_init_attr_ex attr = {
			.cqe = cq_attr->cqe,
			.cq_context = cq_attr->cq_context,
			.channel = cq_attr->comp_channel,
			.comp_vector = cq_attr->comp_vector,
			.wc_flags = IBV_WC_STANDARD_FLAGS | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP,
		};
		struct ibv_device_attr_ex dattr = {};

		if (ibv_query_device_ex(cq_attr->pd->context, NULL, &dattr) == 0 && dattr.hca_core_clock != 0) {
			vcq->ex = ibv_create_cq_ex(cq_attr->pd->context, &attr);
		}
		if (vcq->ex != NULL) {
			vcq->ctx = cq_attr->pd->context;
			vcq->ticks_per_cycle = (double)spdk_get_ticks_hz() / ((double)dattr.hca_core_clock * 1000);
			verbs_cq_sync_clock(vcq, spdk_get_ticks());
			vcq->base.cq = ibv_cq_ex_to_cq(vcq->ex);
			return &vcq->base;
		}
		SPDK_WARNLOG("SPDK_RDMA_CQ_TS: no timestamped CQ (errno %d), using a plain one\n", errno);
	}

	vcq->base.cq = ibv_create_cq(cq_attr->pd->context, cq_attr->cqe, cq_attr->cq_context,
				     cq_attr->comp_channel,
				     cq_attr->comp_vector);
	if (!vcq->base.cq) {
		SPDK_ERRLOG("Unable to create completion queue: errno %d: %s\n", errno, spdk_strerror(errno));
		free(vcq);
		return NULL;
	}

	return &vcq->base;
}

void
spdk_rdma_provider_cq_destroy(struct spdk_rdma_provider_cq *rdma_cq)
{
	struct verbs_cq *vcq = SPDK_CONTAINEROF(rdma_cq, struct verbs_cq, base);

	assert(rdma_cq);

	ibv_destroy_cq(rdma_cq->cq);
	free(vcq);
}

static int
verbs_cq_poll_ts(struct verbs_cq *vcq, int num_entries, struct ibv_wc *wc)
{
	struct ibv_poll_cq_attr attr = {};
	struct ibv_cq_ex *ex = vcq->ex;
	uint64_t now = spdk_get_ticks();
	int n = 0, rc;

	if (now >= vcq->next_sync) {
		verbs_cq_sync_clock(vcq, now);
	}
	if (num_entries > SPDK_RDMA_PROVIDER_CQ_TS_MAX) {
		num_entries = SPDK_RDMA_PROVIDER_CQ_TS_MAX;
	}
	rc = ibv_start_poll(ex, &attr);
	if (rc == ENOENT) {
		g_cq_ts_valid = true;
		return 0;
	} else if (rc != 0) {
		return -rc;
	}
	do {
		struct ibv_wc *w = &wc[n];

		memset(w, 0, sizeof(*w));
		w->wr_id = ex->wr_id;
		w->status = ex->status;
		w->opcode = ibv_wc_read_opcode(ex);
		w->vendor_err = ibv_wc_read_vendor_err(ex);
		w->byte_len = ibv_wc_read_byte_len(ex);
		w->qp_num = ibv_wc_read_qp_num(ex);
		w->src_qp = ibv_wc_read_src_qp(ex);
		w->wc_flags = ibv_wc_read_wc_flags(ex);
		if (w->wc_flags & IBV_WC_WITH_IMM) {
			w->imm_data = ibv_wc_read_imm_data(ex);
		}
		w->slid = ibv_wc_read_slid(ex);
		w->sl = ibv_wc_read_sl(ex);
		w->dlid_path_bits = ibv_wc_read_dlid_path_bits(ex);
		g_cq_ts[n] = (uint64_t)((double)ibv_wc_read_completion_ts(ex) * vcq->ticks_per_cycle + vcq->offset);
		n++;
	} while (n < num_entries && ibv_next_poll(ex) == 0);
	ibv_end_poll(ex);
	g_cq_ts_valid = true;

	return n;
}

int
spdk_rdma_provider_cq_resize(struct spdk_rdma_provider_cq *rdma_cq, int cqe)
{
	assert(rdma_cq);

	return ibv_resize_cq(rdma_cq->cq, cqe);
}

int
spdk_rdma_provider_cq_poll(struct spdk_rdma_provider_cq *rdma_cq, int num_entries,
			   struct ibv_wc *wc)
{
	struct verbs_cq *vcq = SPDK_CONTAINEROF(rdma_cq, struct verbs_cq, base);

	assert(rdma_cq);
	assert(wc);

	if (vcq->ex != NULL) {
		return verbs_cq_poll_ts(vcq, num_entries, wc);
	}
	g_cq_ts_valid = false;
	return ibv_poll_cq(rdma_cq->cq, num_entries, wc);
}

int
spdk_rdma_provider_set_opts(const struct spdk_rdma_provider_opts *opts)
{
	return -ENOTSUP;
}

int
spdk_rdma_provider_get_opts(struct spdk_rdma_provider_opts *opts, size_t opts_size)
{
	return -ENOTSUP;
}

struct spdk_json_write_ctx;

void
spdk_rdma_provider_subsystem_config_json(struct spdk_json_write_ctx *w)
{

}

void
spdk_rdma_provider_memory_key_get_ref(void *mkey)
{

}

void
spdk_rdma_provider_memory_key_put_ref(void *mkey)
{

}

uint32_t
spdk_rdma_provider_memory_key_get_key(void *mkey)
{
	return UINT32_MAX;
}

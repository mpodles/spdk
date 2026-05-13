/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <rdma/rdma_cma.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/dma.h"

#include "spdk_internal/mlx5.h"
#include "spdk_internal/rdma_provider.h"
#include "spdk/log.h"
#include "spdk/util.h"

struct spdk_rdma_mlx5_dv_qp {
	struct spdk_rdma_provider_qp common;
	struct spdk_mlx5_qp *mlx5_qp;
	int send_err;
	int recv_err;
	struct spdk_memory_domain_rdma_ctx domain_ctx;
};

struct mlx5_dv_cq {
	struct spdk_rdma_provider_cq rdma_cq;
	struct spdk_mlx5_cq *mlx5_cq;
};

struct mlx5_dv_srq {
	struct spdk_rdma_provider_srq rdma_srq;
	struct spdk_mlx5_srq *mlx5_srq;
	int recv_err;
};

static struct spdk_rdma_provider_opts g_mlx5_dv_opts = {
	.opts_size = sizeof(struct spdk_rdma_provider_opts),
	.support_offload_on_qp = false,
};

struct spdk_rdma_provider_qp *
spdk_rdma_provider_qp_create(struct rdma_cm_id *cm_id,
			     struct spdk_rdma_provider_qp_init_attr *qp_attr)
{
	assert(cm_id);
	assert(qp_attr);

	struct spdk_memory_domain_ctx ctx = {};
	struct spdk_rdma_mlx5_dv_qp *dv_qp;
	struct mlx5_dv_srq *dv_srq;
	struct spdk_mlx5_qp_attr mlx5_qp_attr = {
		.cap = qp_attr->cap,
		.qp_context = qp_attr->qp_context
	};
	struct mlx5_dv_cq *dv_cq = SPDK_CONTAINEROF(qp_attr->cq, struct mlx5_dv_cq, rdma_cq);
	struct ibv_pd *pd = qp_attr->pd ? qp_attr->pd : cm_id->pd;
	int rc;

	assert(pd);

	if (g_mlx5_dv_opts.support_offload_on_qp) {
		mlx5_qp_attr.cap.max_send_wr *= 2;
	}

	dv_qp = calloc(1, sizeof(*dv_qp));
	if (!dv_qp) {
		SPDK_ERRLOG("qp memory allocation failed\n");
		return NULL;
	}

	if (qp_attr->stats) {
		dv_qp->common.stats = qp_attr->stats;
		dv_qp->common.shared_stats = true;
	} else {
		dv_qp->common.stats = calloc(1, sizeof(*dv_qp->common.stats));
		if (!dv_qp->common.stats) {
			SPDK_ERRLOG("qp statistics memory allocation failed\n");
			free(dv_qp);
			return NULL;
		}
	}

	if (qp_attr->srq) {
		dv_srq = SPDK_CONTAINEROF(qp_attr->srq, struct mlx5_dv_srq, rdma_srq);
		mlx5_qp_attr.srq = dv_srq->mlx5_srq;
	}

	rc = spdk_mlx5_qp_create(pd, dv_cq->mlx5_cq, &mlx5_qp_attr, &dv_qp->mlx5_qp);
	if (rc) {
		SPDK_ERRLOG("Failed to create qpair, rc %d\n", rc);
		free(dv_qp);
		return NULL;
	}

	dv_qp->common.qp = spdk_mlx5_qp_get_verbs_qp(dv_qp->mlx5_qp);
	dv_qp->common.cm_id = cm_id;

	qp_attr->cap = mlx5_qp_attr.cap;

	dv_qp->domain_ctx.size = sizeof(dv_qp->domain_ctx);
	dv_qp->domain_ctx.ibv_pd = qp_attr->pd;
	if (g_mlx5_dv_opts.support_offload_on_qp) {
		dv_qp->domain_ctx.qp = dv_qp->mlx5_qp;
	}

	ctx.size = sizeof(ctx);
	ctx.user_ctx = &dv_qp->domain_ctx;
	ctx.user_ctx_size = dv_qp->domain_ctx.size;
	rc = spdk_memory_domain_create(&dv_qp->common.domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
				       SPDK_RDMA_DMA_DEVICE);
	if (rc) {
		SPDK_ERRLOG("Failed to create memory domain\n");
		spdk_rdma_provider_qp_destroy(&dv_qp->common);
		return NULL;
	}
	if (qp_attr->domain_transfer) {
		spdk_memory_domain_set_data_transfer(dv_qp->common.domain, qp_attr->domain_transfer);
	}

	return &dv_qp->common;
}

int
spdk_rdma_provider_qp_accept(struct spdk_rdma_provider_qp *spdk_rdma_qp,
			     struct rdma_conn_param *conn_param)
{
	struct spdk_rdma_mlx5_dv_qp *dv_qp;

	assert(spdk_rdma_qp != NULL);
	assert(spdk_rdma_qp->cm_id != NULL);

	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	/* NVMEoF target must move qpair to RTS state */
	if (spdk_mlx5_qp_connect_cm(dv_qp->mlx5_qp, spdk_rdma_qp->cm_id) != 0) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		/* Set errno to be compliant with rdma_accept behaviour */
		errno = ECONNABORTED;
		return -1;
	}

	return rdma_accept(spdk_rdma_qp->cm_id, conn_param);
}

int
spdk_rdma_provider_qp_complete_connect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *dv_qp;
	int rc;

	assert(spdk_rdma_qp);

	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	rc = spdk_mlx5_qp_connect_cm(dv_qp->mlx5_qp, spdk_rdma_qp->cm_id);
	if (rc) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		return rc;
	}

	rc = rdma_establish(dv_qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("rdma_establish failed, errno %s (%d)\n", spdk_strerror(errno), errno);
	}

	return rc;
}

void
spdk_rdma_provider_qp_destroy(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *dv_qp;

	assert(spdk_rdma_qp != NULL);

	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_rdma_qp->send_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (!dv_qp->common.shared_stats) {
		free(dv_qp->common.stats);
	}

	if (spdk_rdma_qp->domain) {
		spdk_memory_domain_destroy(spdk_rdma_qp->domain);
	}

	if (dv_qp->mlx5_qp) {
		spdk_mlx5_qp_destroy(dv_qp->mlx5_qp);
	}

	free(dv_qp);
}

int
spdk_rdma_provider_qp_disconnect(struct spdk_rdma_provider_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *dv_qp;
	int rc = 0;

	assert(spdk_rdma_qp != NULL);
	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);
	if (dv_qp->mlx5_qp) {
		struct ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};

		rc = spdk_mlx5_qp_modify(dv_qp->mlx5_qp, &qp_attr, IBV_QP_STATE);
		if (rc) {
			SPDK_WARNLOG("Failed to modify qp %p state to ERR, rc %d\n", dv_qp->mlx5_qp, rc);
			return rc;
		}
	}

	if (spdk_rdma_qp->cm_id) {
		rc = rdma_disconnect(spdk_rdma_qp->cm_id);
		if (rc) {
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}

	return rc;
}

static inline uint32_t
rdma_send_flags_to_mlx5(unsigned int send_flags)
{
	uint32_t mlx5_flags = 0;

	assert((send_flags & ~(IBV_SEND_FENCE | IBV_SEND_SIGNALED | IBV_SEND_SOLICITED)) == 0);

	if (send_flags & IBV_SEND_FENCE) {
		mlx5_flags |= SPDK_MLX5_WQE_CTRL_FENCE;
	}
	if (send_flags & IBV_SEND_SIGNALED) {
		mlx5_flags |= SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE;
	}
	if (send_flags & IBV_SEND_SOLICITED) {
		mlx5_flags |= SPDK_MLX5_WQE_CTRL_SOLICITED;
	}

	return mlx5_flags;
}

static inline int
rdma_qp_queue_send_wr(struct spdk_mlx5_qp *mlx5_qp, struct ibv_send_wr *wr)
{
	int rc;
	uint32_t flags = rdma_send_flags_to_mlx5(wr->send_flags);

	switch (wr->opcode) {
	case IBV_WR_SEND:
		rc = spdk_mlx5_qp_send(mlx5_qp, wr->sg_list, wr->num_sge, wr->wr_id, flags);
		break;
	case IBV_WR_SEND_WITH_INV:
		rc = spdk_mlx5_qp_send_inv(mlx5_qp, wr->sg_list, wr->num_sge, wr->invalidate_rkey,
					   wr->wr_id, flags);
		break;
	case IBV_WR_RDMA_READ:
		rc = spdk_mlx5_qp_rdma_read(mlx5_qp, wr->sg_list, wr->num_sge, wr->wr.rdma.remote_addr,
					    wr->wr.rdma.rkey, wr->wr_id, flags);
		break;
	case IBV_WR_RDMA_WRITE:
		rc = spdk_mlx5_qp_rdma_write(mlx5_qp, wr->sg_list, wr->num_sge, wr->wr.rdma.remote_addr,
					     wr->wr.rdma.rkey, wr->wr_id, flags);
		break;
	default:
		SPDK_ERRLOG("Unexpected opcode %d\n", wr->opcode);
		rc = -EINVAL;
		assert(0);
	}

	return rc;
}

bool
spdk_rdma_provider_qp_queue_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr *first)
{
	struct ibv_send_wr *tmp;
	struct spdk_rdma_mlx5_dv_qp *dv_qp;
	bool is_first;

	assert(spdk_rdma_qp);
	assert(first);

	is_first = spdk_rdma_qp->send_wrs.first == NULL;
	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_unlikely(spdk_rdma_qp->xrc_mode)) {
		/*
		 * XRC send path: stamp remote_srqn on every WR, then submit the whole
		 * chain immediately via ibv_post_send (handles doorbell internally).
		 * We do NOT enqueue into send_wrs — ibv_post_send owns submission.
		 */
		struct ibv_send_wr *bad_wr = NULL;
		int xrc_rc;

		for (tmp = first; tmp != NULL; tmp = tmp->next) {
			tmp->qp_type.xrc.remote_srqn = spdk_rdma_qp->remote_srqn;
			spdk_rdma_qp->stats->send.num_submitted_wrs++;
		}
		xrc_rc = ibv_post_send(spdk_rdma_qp->qp, first, &bad_wr);
		if (spdk_unlikely(xrc_rc)) {
			SPDK_ERRLOG("XRC ibv_post_send failed rc=%d errno=%d\n", xrc_rc, errno);
			dv_qp->send_err = xrc_rc;
		}
		/* send_wrs chain is consumed; do not let flush_send_wrs ring MLX5 doorbell */
		spdk_rdma_qp->send_wrs.first = NULL;
		spdk_rdma_qp->send_wrs.last  = NULL;
		return is_first;
	}

	if (is_first) {
		spdk_rdma_qp->send_wrs.first = first;
	} else {
		spdk_rdma_qp->send_wrs.last->next = first;
	}

	for (tmp = first; tmp != NULL; tmp = tmp->next) {

		if (spdk_likely(!dv_qp->send_err)) {
			dv_qp->send_err = rdma_qp_queue_send_wr(dv_qp->mlx5_qp, tmp);
		}

		spdk_rdma_qp->send_wrs.last = tmp;
		spdk_rdma_qp->stats->send.num_submitted_wrs++;
	}

	return is_first;
}

int
spdk_rdma_provider_qp_flush_send_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_send_wr **bad_wr)
{
	struct spdk_rdma_mlx5_dv_qp *dv_qp;

	assert(bad_wr);
	assert(spdk_rdma_qp);

	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_unlikely(spdk_rdma_qp->xrc_mode)) {
		/*
		 * XRC sends were already submitted (and doorbell rung) inside
		 * queue_send_wrs via ibv_post_send.  Nothing left to flush.
		 */
		return dv_qp->send_err;
	}

	if (spdk_unlikely(spdk_rdma_qp->send_wrs.first == NULL)) {
		return 0;
	}

	if (spdk_unlikely(dv_qp->send_err)) {
		/* If send_err is not zero that means that no WRs are posted to NIC */
		*bad_wr = spdk_rdma_qp->send_wrs.first;
	} else {
		spdk_mlx5_qp_complete_send(dv_qp->mlx5_qp);
		spdk_rdma_qp->stats->send.doorbell_updates++;
	}
	spdk_rdma_qp->send_wrs.first = NULL;

	return dv_qp->send_err;
}

struct spdk_rdma_provider_srq *
spdk_rdma_provider_srq_create(struct spdk_rdma_provider_srq_init_attr *init_attr)
{
	assert(init_attr);
	assert(init_attr->pd);

	struct mlx5_dv_srq *dv_srq;
	struct spdk_rdma_provider_srq *rdma_srq;
	int rc;

	dv_srq = calloc(1, sizeof(*dv_srq));
	if (!dv_srq) {
		SPDK_ERRLOG("Can't allocate memory for SRQ handle\n");
		return NULL;
	}

	rdma_srq = &dv_srq->rdma_srq;
	if (init_attr->stats) {
		rdma_srq->stats = init_attr->stats;
		rdma_srq->shared_stats = true;
	} else {
		rdma_srq->stats = calloc(1, sizeof(*rdma_srq->stats));
		if (!rdma_srq->stats) {
			SPDK_ERRLOG("SRQ statistics memory allocation failed");
			goto err_free_srq;
		}
	}

	rc = spdk_mlx5_srq_create(init_attr->pd, &init_attr->srq_init_attr,
				  init_attr->xrcd, init_attr->xrc_cq,
				  &dv_srq->mlx5_srq);
	if (rc) {
		SPDK_ERRLOG("Unable to create SRQ, rc %d (%s)\n", rc, spdk_strerror(rc));
		goto err_free_stats;
	}

	/* Populate the verbs SRQ pointer (used by callers for ibv_post_recv / logging) */
	rdma_srq->srq = spdk_mlx5_srq_get_verbs_srq(dv_srq->mlx5_srq);

	if (init_attr->xrcd != NULL) {
		/* Retrieve the hardware SRQ number — needed by remote sides as remote_srqn */
		if (ibv_get_srq_num(rdma_srq->srq, &rdma_srq->srqn) != 0) {
			SPDK_ERRLOG("ibv_get_srq_num failed errno=%d\n", errno);
			spdk_mlx5_srq_destroy(dv_srq->mlx5_srq);
			dv_srq->mlx5_srq = NULL;
			goto err_free_stats;
		}
		SPDK_NOTICELOG("XRC provider SRQ: srq=%p srqn=%u (via ibv_get_srq_num)\n",
			       rdma_srq->srq, rdma_srq->srqn);
	}

	return rdma_srq;

err_free_stats:
	if (!rdma_srq->shared_stats) {
		free(rdma_srq->stats);
	}
err_free_srq:
	free(dv_srq);

	return NULL;
}

int
spdk_rdma_provider_srq_destroy(struct spdk_rdma_provider_srq *rdma_srq)
{
	assert(rdma_srq);

	struct mlx5_dv_srq *dv_srq = SPDK_CONTAINEROF(rdma_srq, struct mlx5_dv_srq, rdma_srq);
	int rc;

	if (!rdma_srq) {
		return 0;
	}

	if (rdma_srq->recv_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying RDMA SRQ with queued recv WRs\n");
	}

	rc = spdk_mlx5_srq_destroy(dv_srq->mlx5_srq);
	if (rc) {
		SPDK_ERRLOG("SRQ destroy failed with %d\n", rc);
	}

	if (!rdma_srq->shared_stats) {
		free(rdma_srq->stats);
	}

	free(dv_srq);

	return rc;
}

bool
spdk_rdma_provider_srq_queue_recv_wrs(struct spdk_rdma_provider_srq *rdma_srq,
				      struct ibv_recv_wr *first)
{
	assert(rdma_srq);
	assert(first);

	struct spdk_rdma_provider_wr_stats *recv_stats = rdma_srq->stats;
	struct spdk_rdma_provider_recv_wr_list *recv_wrs = &rdma_srq->recv_wrs;
	struct mlx5_dv_srq *dv_srq;
	struct ibv_recv_wr *wr;
	bool is_first;

	is_first = recv_wrs->first == NULL;
	if (is_first) {
		recv_wrs->first = first;
	} else {
		recv_wrs->last->next = first;
	}

	dv_srq = SPDK_CONTAINEROF(rdma_srq, struct mlx5_dv_srq, rdma_srq);

	for (wr = first; wr != NULL; wr = wr->next) {
		recv_wrs->last = wr;
		recv_stats->num_submitted_wrs++;

		if (spdk_unlikely(dv_srq->recv_err)) {
			/* Do not post WRs to the SRQ on error. */
			continue;
		}
		dv_srq->recv_err = spdk_mlx5_srq_recv(dv_srq->mlx5_srq, wr->sg_list, wr->num_sge, wr->wr_id);
	}

	return is_first;
}

int
spdk_rdma_provider_srq_flush_recv_wrs(struct spdk_rdma_provider_srq *rdma_srq,
				      struct ibv_recv_wr **bad_wr)
{
	assert(rdma_srq);
	assert(bad_wr);

	struct spdk_rdma_provider_recv_wr_list *recv_wrs = &rdma_srq->recv_wrs;
	struct mlx5_dv_srq *dv_srq;

	if (spdk_unlikely(recv_wrs->first == NULL)) {
		return 0;
	}

	dv_srq = SPDK_CONTAINEROF(rdma_srq, struct mlx5_dv_srq, rdma_srq);
	if (spdk_likely(!dv_srq->recv_err)) {
		spdk_mlx5_srq_complete_recv(dv_srq->mlx5_srq);
	} else {
		*bad_wr = recv_wrs->first;
	}

	recv_wrs->first = NULL;
	rdma_srq->stats->doorbell_updates++;

	return dv_srq->recv_err;
}

bool
spdk_rdma_provider_qp_queue_recv_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_recv_wr *first)
{
	assert(spdk_rdma_qp);
	assert(first);

	bool is_first;
	struct spdk_rdma_mlx5_dv_qp *dv_qp;
	struct spdk_rdma_provider_recv_wr_list *recv_wrs = &spdk_rdma_qp->recv_wrs;
	struct spdk_rdma_provider_wr_stats *recv_stats = &spdk_rdma_qp->stats->recv;
	struct ibv_recv_wr *wr;

	is_first = recv_wrs->first == NULL;
	if (is_first) {
		recv_wrs->first = first;
	} else {
		recv_wrs->last->next = first;
	}

	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	for (wr = first; wr != NULL; wr = wr->next) {
		if (!dv_qp->recv_err) {
			dv_qp->recv_err = spdk_mlx5_qp_recv(dv_qp->mlx5_qp, wr->sg_list, wr->num_sge, wr->wr_id);
		}
		recv_wrs->last = wr;
		recv_stats->num_submitted_wrs++;
	}

	return is_first;
}

int
spdk_rdma_provider_qp_flush_recv_wrs(struct spdk_rdma_provider_qp *spdk_rdma_qp,
				     struct ibv_recv_wr **bad_wr)
{
	struct spdk_rdma_mlx5_dv_qp *dv_qp;

	if (spdk_unlikely(spdk_rdma_qp->recv_wrs.first == NULL)) {
		return 0;
	}

	dv_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);
	if (spdk_likely(!dv_qp->recv_err)) {
		spdk_mlx5_qp_complete_recv(dv_qp->mlx5_qp);
	} else {
		*bad_wr = spdk_rdma_qp->recv_wrs.first;
	}

	spdk_rdma_qp->recv_wrs.first = NULL;
	spdk_rdma_qp->stats->recv.doorbell_updates++;

	return dv_qp->recv_err;
}

bool
spdk_rdma_provider_accel_sequence_supported(void)
{
	return spdk_mlx5_umr_implementer_is_registered();
}

struct spdk_rdma_provider_cq *
spdk_rdma_provider_cq_create(struct spdk_rdma_provider_cq_init_attr *cq_attr)
{
	struct mlx5_dv_cq *dv_cq;
	struct spdk_mlx5_cq_attr mlx5_cq_attr = {
		.cqe_cnt = cq_attr->cqe,
		.cqe_size = 64,
		.cq_context = cq_attr->cq_context,
		.comp_channel = cq_attr->comp_channel,
		.comp_vector = cq_attr->comp_vector
	};
	int rc;

	dv_cq = calloc(1, sizeof(*dv_cq));
	if (!dv_cq) {
		SPDK_ERRLOG("CQ memory allocation failed\n");
		return NULL;
	}

	rc = spdk_mlx5_cq_create(cq_attr->pd, &mlx5_cq_attr, &dv_cq->mlx5_cq);
	if (rc) {
		SPDK_ERRLOG("Failed to create CQ, rc %d\n", rc);
		free(dv_cq);
		return NULL;
	}

	return &dv_cq->rdma_cq;
}

void
spdk_rdma_provider_cq_destroy(struct spdk_rdma_provider_cq *rdma_cq)
{
	assert(rdma_cq);

	struct mlx5_dv_cq *dv_cq = SPDK_CONTAINEROF(rdma_cq, struct mlx5_dv_cq, rdma_cq);

	spdk_mlx5_cq_destroy(dv_cq->mlx5_cq);
	free(dv_cq);
}

int
spdk_rdma_provider_cq_resize(struct spdk_rdma_provider_cq *rdma_cq, int cqe)
{
	assert(rdma_cq);

	struct mlx5_dv_cq *dv_cq = SPDK_CONTAINEROF(rdma_cq, struct mlx5_dv_cq, rdma_cq);

	return spdk_mlx5_cq_resize(dv_cq->mlx5_cq, cqe);
}

int
spdk_rdma_provider_cq_poll(struct spdk_rdma_provider_cq *rdma_cq, int num_entries,
			   struct ibv_wc *wc)
{
	assert(rdma_cq);
	assert(wc);

	struct mlx5_dv_cq *dv_cq = SPDK_CONTAINEROF(rdma_cq, struct mlx5_dv_cq, rdma_cq);

	return spdk_mlx5_cq_poll_wc(dv_cq->mlx5_cq, num_entries, wc);
}

int
spdk_rdma_provider_set_opts(const struct spdk_rdma_provider_opts *opts)
{
	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -EINVAL;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -EINVAL;
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_rdma_provider_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_mlx5_dv_opts.field = opts->field; \
        } \

	SET_FIELD(support_offload_on_qp);

	g_mlx5_dv_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	return 0;
}

int
spdk_rdma_provider_get_opts(struct spdk_rdma_provider_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return -EINVAL;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return -EINVAL;
	}

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
	if (offsetof(struct spdk_rdma_provider_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_mlx5_dv_opts.field; \
	}

	SET_FIELD(support_offload_on_qp);

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_rdma_provider_opts) == 16, "Incorrect size");

#undef SET_FIELD
	return 0;
}

void
spdk_rdma_provider_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	assert(w != NULL);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "rdma_provider_set_opts");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_bool(w, "support_offload_on_qp", g_mlx5_dv_opts.support_offload_on_qp);
	spdk_json_write_object_end(w); /* params */
	spdk_json_write_object_end(w);
}

void
spdk_rdma_provider_memory_key_get_ref(void *mkey)
{
	spdk_mlx5_mkey_pool_obj_get_ref((struct spdk_mlx5_mkey_pool_obj *)mkey);
}

void
spdk_rdma_provider_memory_key_put_ref(void *mkey)
{
	spdk_mlx5_mkey_pool_obj_put_ref((struct spdk_mlx5_mkey_pool_obj *)mkey);
}

uint32_t
spdk_rdma_provider_memory_key_get_key(void *_mkey)
{

	struct spdk_mlx5_mkey_pool_obj *mkey = _mkey;

	return mkey->mkey;
}

SPDK_LOG_REGISTER_COMPONENT(rdma_mlx5_dv)

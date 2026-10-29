/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe-oF/RDMA over XRC: the non-standard wire format both ends of this fork
 * use when XRC is enabled on both sides (target transport option "xrc",
 * host transport option rdma_xrc). Admin queues stay RC and never carry it.
 *
 * Topology (W target poll groups, one host):
 *   host I/O queue q:  XRC INI QP q  --commands-->  target XRC TGT QP q
 *                      (the SEND names one of the W target command SRQs)
 *   target poll group w: one XRC INI QP per host  --READ/WRITE/response-->
 *                      host XRC TGT QP w (one per target poll group)
 *                      (responses name the SRQ of the host poller that owns q)
 *
 * Connection setup, per host I/O queue, on top of the usual RDMA-CM
 * exchange. The CM pairs host INI QP q with target TGT QP q: their QPNs and
 * PSNs travel in the CM REQ/REP as for RC, and both sides take the path from
 * rdma_init_qp_attr(). The rest:
 *
 *  1. CM REQ private data (host -> target):
 *     spdk_nvmf_rdma_xrc_request_private_data. The session id groups all I/O
 *     queues of one host incarnation (one host NQN + host ID, chosen at random
 *     by the host each time it creates its XRC resources for a controller).
 *  2. CM REP private data (target -> host):
 *     spdk_nvmf_rdma_xrc_accept_private_data + W command SRQNs + W target INI
 *     QPNs (one INI QP per (poll group, session)). The target creates the INI
 *     QPs when the session's first queue connects.
 *  3. The host creates one XRC TGT QP per target poll group (first queue of
 *     the session only), connects each to the matching target INI QP, moves
 *     INI QP q to RTS and calls rdma_establish().
 *  4. Fabrics Connect data (host -> target), reserved bytes at offset 18:
 *     spdk_nvmf_rdma_xrc_connect_ext with the host TGT QPNs. The target
 *     connects its INI QPs when it processes the first Fabrics Connect of the
 *     session, before it sends that Connect's response. Connect data must
 *     travel in the capsule: the INI QPs that could RDMA-READ it are not
 *     connected yet.
 *
 * A session never recovers from a failed INI or TGT QP. The target marks it
 * failed and rejects new queues for it; the host stops adding queues to it
 * (after a TGT QP error, a CM reject or a changed REP) and starts a new
 * session with a new id. Queues already on the old session keep working
 * until they disconnect.
 *
 * QPs connected outside RDMA-CM (target INI w <-> host TGT w) use
 * PSN = INI QPN & 0xffffff in both directions' bookkeeping.
 */

#ifndef SPDK_INTERNAL_NVMF_RDMA_XRC_H
#define SPDK_INTERNAL_NVMF_RDMA_XRC_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#define SPDK_NVMF_RDMA_XRC_MAGIC	0x5852	/* "XR" */
/* 2: the target may give each poll group a second INI QP for large transfers. */
#define SPDK_NVMF_RDMA_XRC_VERSION	2

/* REP private data is at most 196 bytes: 32 + 4 * W SRQNs + 4 * (INI QPs). */
#define SPDK_NVMF_RDMA_XRC_REP_MAX	196
#define SPDK_NVMF_RDMA_XRC_MAX_WORKERS	20
/* INI QPs per session: one per poll group, or two (small and large transfers). */
#define SPDK_NVMF_RDMA_XRC_MAX_INI	(2 * SPDK_NVMF_RDMA_XRC_MAX_WORKERS)

/* SQHD sent in every XRC response: commands of one queue complete on several
 * cores, and the SPDK host ignores SQHD on RDMA. */
#define SPDK_NVMF_RDMA_XRC_SQHD		0xFFFF

/* Steering policies of the host (spdk_nvme_transport_opts.rdma_xrc_steering). */
enum spdk_nvme_rdma_xrc_steering {
	/* Every command to the queue's own target poll group (stock placement). */
	SPDK_NVME_RDMA_XRC_STEER_HOME	= 0,
	/* Round-robin over the target poll groups, per queue. */
	SPDK_NVME_RDMA_XRC_STEER_RR	= 1,
	/* Uniformly random target poll group per command. */
	SPDK_NVME_RDMA_XRC_STEER_RANDOM	= 2,
};

/*
 * The host already holds TGT QPs connected to this session's INI QPs. The
 * target rejects such a request when it no longer knows the session (all its
 * queues are gone, or the target restarted): the host then starts a new
 * session instead of joining INI QPs it is not connected to.
 */
#define SPDK_NVMF_RDMA_XRC_REQ_JOIN	0x01

struct __attribute__((packed)) spdk_nvmf_rdma_xrc_request_private_data {
	/* Same first 10 bytes as spdk_nvmf_rdma_request_private_data. */
	uint16_t	recfmt;
	uint16_t	qid;
	uint16_t	hrqsize;
	uint16_t	hsqsize;
	uint16_t	cntlid;
	/* XRC extension, in the standard struct's reserved bytes. */
	uint16_t	xrc_magic;
	uint8_t		xrc_version;
	/* SPDK_NVMF_RDMA_XRC_REQ_* */
	uint8_t		flags;
	/* SRQ of the host poller owning this queue: responses go there. */
	uint32_t	resp_srqn;
	uint64_t	session_id;
	uint8_t		reserved2[6];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_xrc_request_private_data) == 32,
		   "Incorrect size");

struct __attribute__((packed)) spdk_nvmf_rdma_xrc_accept_private_data {
	/* Same first 4 bytes as spdk_nvmf_rdma_accept_private_data. */
	uint16_t	recfmt;
	uint16_t	crqsize;
	uint16_t	xrc_magic;
	uint8_t		xrc_version;
	uint8_t		num_workers;
	/* Index of the poll group that owns this queue (its home SRQ). */
	uint8_t		home;
	/* INI QPs: num_workers, or 2 * num_workers when transfers above the target's
	 * threshold go on a second INI QP per poll group (ini_qpn[num_workers + w]). */
	uint8_t		num_ini;
	uint8_t		reserved1[2];
	uint32_t	tgt_qpn;
	uint8_t		reserved2[16];
	/* Followed by uint32_t srqn[num_workers], uint32_t ini_qpn[num_ini]. */
	uint32_t	data[];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_xrc_accept_private_data) == 32,
		   "Incorrect size");

/* Lives in spdk_nvmf_fabric_connect_data.reserved5 (offset 18, 238 bytes). */
struct __attribute__((packed)) spdk_nvmf_rdma_xrc_connect_ext {
	uint16_t	xrc_magic;
	uint8_t		xrc_version;
	uint8_t		num_tgt;
	uint64_t	session_id;
	/* One host TGT QP per target INI QP, in the order of the accept data. */
	uint32_t	tgt_qpn[SPDK_NVMF_RDMA_XRC_MAX_INI];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_rdma_xrc_connect_ext) <= 238, "Incorrect size");

#define SPDK_NVMF_RDMA_XRC_CONNECT_EXT_OFFSET	18

static inline uint32_t
spdk_nvmf_rdma_xrc_psn(uint32_t ini_qpn)
{
	return ini_qpn & 0xffffff;
}

#endif /* SPDK_INTERNAL_NVMF_RDMA_XRC_H */

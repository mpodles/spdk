/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/trace.h"
#include "spdk/tree.h"
#include "spdk/util.h"

#include "spdk_internal/assert.h"
#include "spdk/log.h"
#include "spdk_internal/rdma_provider.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk_internal/nvmf_rdma_xrc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk_internal/trace_defs.h"

struct spdk_nvme_rdma_hooks g_nvmf_hooks = {};
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma;

/*
 RDMA Connection Resource Defaults
 */
#define NVMF_DEFAULT_MSDBD		16
#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES
#define NVMF_DEFAULT_RSP_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

#define NVMF_RDMA_MAX_EVENTS_PER_POLL	32

SPDK_STATIC_ASSERT(NVMF_DEFAULT_MSDBD <= SPDK_NVMF_MAX_SGL_ENTRIES,
		   "MSDBD must not exceed SPDK_NVMF_MAX_SGL_ENTRIES");

/* The RDMA completion queue size */
#define DEFAULT_NVMF_RDMA_CQ_SIZE	4096
#define MAX_WR_PER_QP(queue_depth)	(queue_depth * 3 + 2)

enum spdk_nvmf_rdma_request_state {
	/* The request is not currently in use */
	RDMA_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	RDMA_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	RDMA_REQUEST_STATE_NEED_BUFFER,

	/* The request is queued until accel task is available. */
	RDMA_REQUEST_STATE_NEED_ACCEL_TASK,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the host to the controller.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,

	/* The request is currently transferring data from the host to the controller. */
	RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	RDMA_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	RDMA_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	RDMA_REQUEST_STATE_EXECUTED,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the controller to the host.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,

	/* The request is waiting on RDMA queue depth availability
	 * to send response to the host.
	 */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING,

	/* The request is ready to send a completion */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request is currently transferring data from the controller to the host. */
	RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,

	/* The request currently has an outstanding completion without an
	 * associated data transfer.
	 */
	RDMA_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	RDMA_REQUEST_STATE_COMPLETED,

	/* Terminator */
	RDMA_REQUEST_NUM_STATES,
};

static void
nvmf_trace(void)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');

	struct spdk_trace_tpoint_opts opts[] = {
		{
			"RDMA_REQ_NEW", TRACE_RDMA_REQUEST_STATE_NEW,
			OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 1,
			{
				{ "qpair", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
		{
			"RDMA_REQ_COMPLETED", TRACE_RDMA_REQUEST_STATE_COMPLETED,
			OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
			{
				{ "qpair", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER", TRACE_RDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_NEED_ACCEL", TRACE_RDMA_REQUEST_STATE_NEED_ACCEL_TASK,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_C2H",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H2C",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_H2C",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE",
					TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_EXECUTING",
					TRACE_RDMA_REQUEST_STATE_EXECUTING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_EXECUTED",
					TRACE_RDMA_REQUEST_STATE_EXECUTED,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY2COMPL_PEND",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPL",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETING_C2H",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETING",
					TRACE_RDMA_REQUEST_STATE_COMPLETING,
					OWNER_TYPE_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");

	spdk_trace_register_description("RDMA_QP_CREATE", TRACE_RDMA_QP_CREATE,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("RDMA_IBV_ASYNC_EVENT", TRACE_RDMA_IBV_ASYNC_EVENT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");
	spdk_trace_register_description("RDMA_CM_ASYNC_EVENT", TRACE_RDMA_CM_ASYNC_EVENT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");
	spdk_trace_register_description("RDMA_QP_DISCONNECT", TRACE_RDMA_QP_DISCONNECT,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("RDMA_QP_DESTROY", TRACE_RDMA_QP_DESTROY,
					OWNER_TYPE_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");

	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_NVMF_RDMA_IO, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_NVMF_RDMA_IO, 0);
}
SPDK_TRACE_REGISTER_FN(nvmf_trace, "nvmf_rdma", TRACE_GROUP_NVMF_RDMA)

enum spdk_nvmf_rdma_wr_type {
	RDMA_WR_TYPE_RECV,
	RDMA_WR_TYPE_SEND,
	RDMA_WR_TYPE_DATA,
};

struct spdk_nvmf_rdma_wr {
	/* Uses enum spdk_nvmf_rdma_wr_type */
	uint8_t type;
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr			wr;
	struct ibv_sge				sgl[NVMF_DEFAULT_RX_SGE];

	struct spdk_nvmf_rdma_qpair		*qpair;

	/* The SRQ this receive is posted to, NULL without SRQ. */
	struct spdk_rdma_provider_srq		*srq;

	/* In-capsule data buffer */
	uint8_t					*buf;

	struct spdk_nvmf_rdma_wr		rdma_wr;
	uint64_t				receive_tsc;

	STAILQ_ENTRY(spdk_nvmf_rdma_recv)	link;
};

struct spdk_nvmf_rdma_request_data {
	struct ibv_send_wr		wr;
	struct ibv_sge			sgl[SPDK_NVMF_MAX_SGL_ENTRIES];
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;

	bool					fused_failed;
	bool					data_transferred;

	struct spdk_nvmf_rdma_wr		data_wr;
	struct spdk_nvmf_rdma_wr		rsp_wr;

	/* Uses enum spdk_nvmf_rdma_request_state */
	uint8_t					state;

	/* Data offset in req.iov */
	uint32_t				offset;

	struct spdk_nvmf_rdma_recv		*recv;

	struct {
		struct	ibv_send_wr		wr;
		struct	ibv_sge			sgl[NVMF_DEFAULT_RSP_SGE];
	} rsp;

	uint16_t				iovpos;
	uint16_t				num_outstanding_data_wr;
	/* Used to split Write IO with multi SGL payload */
	uint16_t				num_remaining_data_wr;
	uint64_t				receive_tsc;
	struct spdk_nvmf_rdma_request		*fused_pair;
	STAILQ_ENTRY(spdk_nvmf_rdma_request)	state_link;
	struct ibv_send_wr			*remaining_tranfer_in_wrs;
	struct ibv_send_wr			*transfer_wr;
	struct spdk_nvmf_rdma_request_data	data;
	void					*data_transfer_mkey;
	spdk_memory_domain_data_cpl_cb		transfer_cpl_cb;
	void					*transfer_cpl_cb_arg;
};

struct spdk_nvmf_rdma_resource_opts {
	struct spdk_nvmf_rdma_qpair	*qpair;
	/* qp points either to an ibv_qp object or an ibv_srq object depending on the value of shared. */
	void				*qp;
	struct spdk_rdma_utils_mem_map	*map;
	uint32_t			max_queue_depth;
	uint32_t			in_capsule_data_size;
	bool				shared;
};

struct spdk_nvmf_rdma_resources {
	/* Array of size "max_queue_depth" containing RDMA requests. */
	struct spdk_nvmf_rdma_request		*reqs;

	/* Array of size "max_queue_depth" containing RDMA recvs. */
	struct spdk_nvmf_rdma_recv		*recvs;

	/* Array of size "max_queue_depth" containing 64 byte capsules
	 * used for receive.
	 */
	union nvmf_h2c_msg			*cmds;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void					*bufs;

	/* Receives that are waiting for a request object */
	STAILQ_HEAD(, spdk_nvmf_rdma_recv)	incoming_queue;

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_rdma_request)	free_queue;
};

/*
 * Send side of an RDMA QP: the WR batch being built, the send and RDMA READ
 * depth and the requests waiting for either. On RC every qpair owns one
 * (spdk_nvmf_rdma_qpair.own_sq). Under XRC one belongs to each INI QP
 * (poll group, host) and is shared by every queue of that host whose commands
 * the poll group runs, so data and response of a command always leave on the
 * QP of the core that ran it.
 */
struct spdk_nvmf_rdma_sq {
	struct spdk_rdma_provider_qp		*rdma_qp;

	/* The maximum number of active RDMA READ and ATOMIC operations at one time */
	uint16_t				max_read_depth;

	/* The current number of active RDMA READ operations */
	uint16_t				current_read_depth;

	/* The maximum number of RDMA SEND operations at one time */
	uint32_t				max_send_depth;

	/* The current number of posted WRs from this send queue.
	 * Should not exceed max_send_depth. */
	uint32_t				current_send_depth;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_read_queue;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_write_queue;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_send_queue;

	STAILQ_ENTRY(spdk_nvmf_rdma_sq)		send_link;

	/* Requests in pending_rdma_read_queue now and at most, and how often its
	 * head waited for READ credits or send slots. */
	uint32_t				pending_read_len;
	uint32_t				pending_read_peak;
	uint64_t				read_stalls;

	/* XRC only: the INI QP this belongs to, NULL on RC. */
	struct spdk_nvmf_rdma_xrc_ini		*xrc_ini;
};

struct spdk_nvmf_rdma_xrc_qpair;
struct spdk_nvmf_rdma_xrc_ini;

#define NVMF_RDMA_SQ_READ_INSERT(_sq, _req) do {						\
	STAILQ_INSERT_TAIL(&(_sq)->pending_rdma_read_queue, (_req), state_link);		\
	if (++(_sq)->pending_read_len > (_sq)->pending_read_peak) {				\
		(_sq)->pending_read_peak = (_sq)->pending_read_len;				\
	}											\
} while (0)

#define NVMF_RDMA_SQ_READ_REMOVE(_sq, _req) do {						\
	STAILQ_REMOVE(&(_sq)->pending_rdma_read_queue, (_req), spdk_nvmf_rdma_request, state_link); \
	(_sq)->pending_read_len--;								\
} while (0)

#define NVMF_RDMA_SQ_READ_REMOVE_HEAD(_sq) do {						\
	STAILQ_REMOVE_HEAD(&(_sq)->pending_rdma_read_queue, state_link);			\
	(_sq)->pending_read_len--;								\
} while (0)

typedef void (*spdk_nvmf_rdma_qpair_ibv_event)(struct spdk_nvmf_rdma_qpair *rqpair);

typedef void (*spdk_poller_destroy_cb)(void *ctx);

struct spdk_nvmf_rdma_ibv_event_ctx {
	struct spdk_nvmf_rdma_qpair			*rqpair;
};

struct spdk_nvmf_rdma_qpair {
	struct spdk_nvmf_qpair			qpair;

	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;

	struct spdk_rdma_provider_qp		*rdma_qp;
	struct rdma_cm_id			*cm_id;
	struct spdk_rdma_provider_srq		*srq;
	struct rdma_cm_id			*listen_id;

	/* Cache the QP number to improve QP search by RB tree. */
	uint32_t				qp_num;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* The current number of outstanding WRs from this qpair's
	 * recv queue. Should not exceed device->attr.max_queue_depth.
	 */
	uint16_t				current_recv_depth;

	/* The maximum number of SGEs per WR on the send queue */
	uint32_t				max_send_sge;

	struct spdk_nvmf_rdma_resources		*resources;

	/* Send side used by this qpair's requests: &own_sq on RC; under XRC the
	 * send state of the INI QP (poll group, host) that this qpair posts on. */
	struct spdk_nvmf_rdma_sq		*sq;
	struct spdk_nvmf_rdma_sq		own_sq;

	/* XRC state, NULL on RC (see struct spdk_nvmf_rdma_xrc_qpair). */
	struct spdk_nvmf_rdma_xrc_qpair		*xrc;

	/* Number of requests not in the free state */
	uint32_t				qd;

	bool					ibv_in_error_state;
	bool					start_accel_sequence;
	bool					in_destroy;

	RB_ENTRY(spdk_nvmf_rdma_qpair)		node;

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	recv_link;

	/* Points to the a request that has fuse bits set to
	 * SPDK_NVME_CMD_FUSE_FIRST, when the qpair is waiting
	 * for the request that has SPDK_NVME_CMD_FUSE_SECOND.
	 */
	struct spdk_nvmf_rdma_request		*fused_first;

	/*
	 * io_channel which is used to destroy qpair when it is removed from poll group
	 */
	struct spdk_io_channel		*destruct_channel;

	/* ctx for async processing of last_wqe_reached event */
	struct spdk_nvmf_rdma_ibv_event_ctx	*last_wqe_reached_ctx;

	/* Lets us know that we have received the last_wqe event. */
	bool					last_wqe_reached;

	/* Indicate that nvmf_rdma_close_qpair is called */
	bool					to_close;
};

struct spdk_nvmf_rdma_poller_stat {
	uint64_t				completions;
	uint64_t				polls;
	uint64_t				idle_polls;
	uint64_t				requests;
	uint64_t				request_latency;
	uint64_t				pending_free_request;
	uint64_t				pending_rdma_read;
	uint64_t				pending_rdma_write;
	uint64_t				pending_rdma_send;
	struct spdk_rdma_provider_qp_stats	qp_stats;
	/* XRC: commands received on this poller's XRC SRQ. */
	uint64_t				xrc_requests;
	/* XRC: receives for a TGT QP with no live queue (reposted, dropped). */
	uint64_t				xrc_orphan_recvs;
	/* XRC: shadow qpairs created / destroyed on this poller. */
	uint64_t				xrc_shadows_created;
	uint64_t				xrc_shadows_destroyed;
};

struct spdk_nvmf_rdma_poller {
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poll_group	*group;

	int					num_cqe;
	int					required_num_wr;
	struct spdk_rdma_provider_cq		*cq;

	/* The maximum number of I/O outstanding on the shared receive queue at one time */
	uint16_t				max_srq_depth;
	bool					need_destroy;

	/* Shared receive queue */
	struct spdk_rdma_provider_srq		*srq;

	struct spdk_nvmf_rdma_resources		*resources;

	/* XRC: the command SRQ of this (poll group, device), its number and
	 * receive resources. Every host queue may send commands here. */
	struct spdk_rdma_provider_srq		*xrc_srq;
	uint32_t				xrc_srqn;
	uint32_t				max_xrc_srq_depth;
	struct spdk_nvmf_rdma_resources		*xrc_resources;
	/* XRC receive buffers taken off the SRQ and not yet reposted. */
	uint32_t				xrc_recvs_held;

	struct spdk_nvmf_rdma_poller_stat	stat;

	spdk_poller_destroy_cb			destroy_cb;
	void					*destroy_cb_ctx;

	RB_HEAD(qpairs_tree, spdk_nvmf_rdma_qpair) qpairs;

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_recv;

	STAILQ_HEAD(, spdk_nvmf_rdma_sq)	qpairs_pending_send;

	TAILQ_ENTRY(spdk_nvmf_rdma_poller)	link;
};

struct spdk_nvmf_rdma_poll_group_stat {
	uint64_t				pending_data_buffer;
	uint64_t				pending_accel_task;
};

struct spdk_nvmf_rdma_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	/* Position in spdk_nvmf_rdma_transport.xrc_pgs (XRC worker index). */
	uint32_t					xrc_index;
	struct spdk_nvmf_rdma_poll_group_stat		stat;
	struct spdk_io_channel				*accel_ch;
	/* Requests that are waiting for accel task */
	STAILQ_HEAD(, spdk_nvmf_rdma_request)		pending_accel_queue;
	TAILQ_HEAD(, spdk_nvmf_rdma_poller)		pollers;
	TAILQ_ENTRY(spdk_nvmf_rdma_poll_group)		link;
};

struct spdk_nvmf_rdma_conn_sched {
	struct spdk_nvmf_rdma_poll_group *next_admin_pg;
	struct spdk_nvmf_rdma_poll_group *next_io_pg;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	struct ibv_context			*context;

	struct spdk_rdma_utils_mem_map		*map;
	struct ibv_pd				*pd;

	int					num_srq;
	bool					need_destroy;
	bool					ready_to_destroy;
	bool					is_ready;

	/* XRC: domain, live XRC queues by TGT QPN, sessions. Guarded by xrc_lock,
	 * which only the control path and shadow creation take. */
	struct ibv_xrcd				*xrcd;
	pthread_mutex_t				xrc_lock;
	RB_HEAD(xrc_tgt_tree, spdk_nvmf_rdma_xrc_qpair) xrc_tgts;
	TAILQ_HEAD(, spdk_nvmf_rdma_xrc_session) xrc_sessions;

	TAILQ_ENTRY(spdk_nvmf_rdma_device)	link;
};

struct spdk_nvmf_rdma_port {
	const struct spdk_nvme_transport_id	*trid;
	struct rdma_cm_id			*id;
	struct spdk_nvmf_rdma_device		*device;
	TAILQ_ENTRY(spdk_nvmf_rdma_port)	link;
};

struct rdma_transport_opts {
	int		num_cqe;
	uint32_t	max_srq_depth;
	bool		no_srq;
	bool		no_wr_batching;
	bool		in_capsule_data_disabled;
	int		acceptor_backlog;
	/* XRC: I/O queues of XRC hosts use XRC; see nvmf_rdma_xrc.h. */
	bool		xrc;
	/* XRC: depth of each poll group's command SRQ. */
	uint32_t	xrc_srq_depth;
	/* XRC: send queue depth of each INI QP (poll group, host). */
	uint32_t	xrc_ini_send_depth;
};

struct spdk_nvmf_rdma_transport {
	struct spdk_nvmf_transport	transport;
	struct rdma_transport_opts	rdma_opts;

	struct spdk_nvmf_rdma_conn_sched conn_sched;

	struct rdma_event_channel	*event_channel;

	struct spdk_mempool		*data_wr_pool;

	struct spdk_poller		*accept_poller;

	/* fields used to poll RDMA/IB events */
	nfds_t			npoll_fds;
	struct pollfd		*poll_fds;

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
	TAILQ_HEAD(, spdk_nvmf_rdma_poll_group)	poll_groups;

	/* ports that are removed unexpectedly and need retry listen */
	TAILQ_HEAD(, spdk_nvmf_rdma_port)		retry_ports;

	/* XRC: poll groups by worker index. */
	struct spdk_nvmf_rdma_poll_group	*xrc_pgs[SPDK_NVMF_RDMA_XRC_MAX_WORKERS];
	uint32_t				xrc_num_pgs;
};

struct poller_manage_ctx {
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_device		*device;

	struct spdk_thread			*thread;
	volatile int				*inflight_op_counter;
};

static const struct spdk_json_object_decoder rdma_transport_opts_decoder[] = {
	{
		"num_cqe", offsetof(struct rdma_transport_opts, num_cqe),
		spdk_json_decode_int32, true
	},
	{
		"max_srq_depth", offsetof(struct rdma_transport_opts, max_srq_depth),
		spdk_json_decode_uint32, true
	},
	{
		"no_srq", offsetof(struct rdma_transport_opts, no_srq),
		spdk_json_decode_bool, true
	},
	{
		"no_wr_batching", offsetof(struct rdma_transport_opts, no_wr_batching),
		spdk_json_decode_bool, true
	},
	{
		"acceptor_backlog", offsetof(struct rdma_transport_opts, acceptor_backlog),
		spdk_json_decode_int32, true
	},
	{
		"xrc", offsetof(struct rdma_transport_opts, xrc),
		spdk_json_decode_bool, true
	},
	{
		"xrc_srq_depth", offsetof(struct rdma_transport_opts, xrc_srq_depth),
		spdk_json_decode_uint32, true
	},
	{
		"xrc_ini_send_depth", offsetof(struct rdma_transport_opts, xrc_ini_send_depth),
		spdk_json_decode_uint32, true
	},
};

static int
nvmf_rdma_qpair_compare(struct spdk_nvmf_rdma_qpair *rqpair1, struct spdk_nvmf_rdma_qpair *rqpair2)
{
	return rqpair1->qp_num < rqpair2->qp_num ? -1 : rqpair1->qp_num > rqpair2->qp_num;
}

RB_GENERATE_STATIC(qpairs_tree, spdk_nvmf_rdma_qpair, node, nvmf_rdma_qpair_compare);

static void nvmf_rdma_qpair_disconnect(struct spdk_nvmf_rdma_qpair *rqpair);

/*
 * ---------------------------------------------------------------------------
 * NVMe-oF/RDMA over XRC (transport option "xrc"). Wire format and topology:
 * include/spdk_internal/nvmf_rdma_xrc.h.
 *
 * A host I/O queue is one XRC TGT QP here, with no receive queue: each command
 * lands in the XRC SRQ of whichever poll group the host named, and that poll
 * group runs it. The queue keeps its "real" spdk_nvmf_rdma_qpair on the poll
 * group SPDK picked at connect time (its home): the generic layer only knows
 * this one (Connect, controller, disconnect). Every other poll group that
 * receives a command of the queue gets a shadow qpair, created on first use on
 * that poll group's thread, holding what must stay thread-local: the group
 * (bdev channels, pause counters), the outstanding list and counters. Data and
 * response go out on the INI QP of (poll group, host session), so they share
 * one QP and the RDMA WRITE-before-SEND ordering holds.
 * ---------------------------------------------------------------------------
 */

enum nvmf_rdma_xrc_session_state {
	NVMF_RDMA_XRC_SESSION_CREATED,
	NVMF_RDMA_XRC_SESSION_CONNECTED,
	NVMF_RDMA_XRC_SESSION_FAILED,
};

/* One XRC INI QP: poll group -> one host session. Owned by its poller's thread. */
struct spdk_nvmf_rdma_xrc_ini {
	struct ibv_qp				*qp;
	struct spdk_rdma_provider_qp		rdma_qp;
	struct spdk_nvmf_rdma_sq		sq;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_xrc_session	*session;
	uint32_t				index;
	uint32_t				host_tgt_qpn;
	bool					in_error;
};

struct spdk_nvmf_rdma_xrc_session {
	uint64_t				id;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_transport		*rtransport;
	/* Real qpairs referencing the session. Guarded by device->xrc_lock. */
	uint32_t				refcnt;
	/* INI QPs not yet destroyed by their owner thread. */
	uint32_t				ini_alive;
	enum nvmf_rdma_xrc_session_state	state;
	struct spdk_uuid			hostid;
	char					hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	/* Path to the host, from the CM REQ of the session's first queue. */
	struct ibv_qp_attr			rtr;
	struct ibv_qp_attr			rts;
	uint8_t					port_num;
	uint16_t				max_read_depth;
	uint32_t				num_ini;
	struct spdk_nvmf_rdma_xrc_ini		ini[SPDK_NVMF_RDMA_XRC_MAX_WORKERS];
	TAILQ_ENTRY(spdk_nvmf_rdma_xrc_session)	link;
};

/* XRC part of an spdk_nvmf_rdma_qpair, real or shadow. */
struct spdk_nvmf_rdma_xrc_qpair {
	struct spdk_nvmf_rdma_xrc_session	*session;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	/* The queue's real qpair (== rqpair unless this is a shadow). */
	struct spdk_nvmf_rdma_qpair		*real;
	bool					is_shadow;
	bool					destroy_scheduled;
	uint32_t				tgt_qpn;
	uint32_t				resp_srqn;
	/* Index of the poll group this qpair lives on. */
	uint32_t				worker;
	/* Commands this (queue, poll group) received. */
	uint64_t				cmds;

	/* Real qpair only. */
	struct ibv_qp				*tgt_qp;
	struct spdk_thread			*home_thread;
	/* qpair_fini callback, held back until the shadows have drained: they
	 * still use the controller, which the generic layer may free once the
	 * real qpair is finalized. */
	spdk_nvmf_transport_qpair_fini_cb	fini_cb;
	void					*fini_cb_arg;
	/* Below guarded by device->xrc_lock. */
	bool					closing;
	bool					in_tree;
	uint32_t				num_shadows;
	struct spdk_nvmf_rdma_qpair		*shadows[SPDK_NVMF_RDMA_XRC_MAX_WORKERS];
	uint64_t				cmds_per_worker[SPDK_NVMF_RDMA_XRC_MAX_WORKERS];
	RB_ENTRY(spdk_nvmf_rdma_xrc_qpair)	tgt_node;
};

static int
nvmf_rdma_xrc_tgt_compare(struct spdk_nvmf_rdma_xrc_qpair *a, struct spdk_nvmf_rdma_xrc_qpair *b)
{
	return a->tgt_qpn < b->tgt_qpn ? -1 : a->tgt_qpn > b->tgt_qpn;
}

RB_GENERATE_STATIC(xrc_tgt_tree, spdk_nvmf_rdma_xrc_qpair, tgt_node, nvmf_rdma_xrc_tgt_compare);

static void nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
		struct spdk_nvmf_rdma_qpair *rqpair, bool drain);
static bool nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_request *rdma_req);
static void nvmf_rdma_destroy_drained_qpair(struct spdk_nvmf_rdma_qpair *rqpair);
static struct spdk_nvmf_rdma_resources *nvmf_rdma_resources_create(
	struct spdk_nvmf_rdma_resource_opts *opts);
static int _nvmf_rdma_qpair_abort_request(void *ctx);
static int nvmf_rdma_request_free(struct spdk_nvmf_request *req);

static inline bool
nvmf_rdma_is_xrc_shadow(struct spdk_nvmf_rdma_qpair *rqpair)
{
	return rqpair->xrc != NULL && rqpair->xrc->is_shadow;
}

/* Number of request objects a qpair's requests may come from. */
static inline uint32_t
nvmf_rdma_qpair_max_reqs(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->xrc != NULL) {
		return rqpair->poller->max_xrc_srq_depth;
	}
	return rqpair->srq == NULL ? rqpair->max_queue_depth : rqpair->poller->max_srq_depth;
}

/* Every WR on an INI QP names the host poller SRQ of the request's queue:
 * the host NIC checks a READ/WRITE rkey against that SRQ's PD. */
static inline void
nvmf_rdma_xrc_stamp(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_send_wr *wr)
{
	if (rqpair->xrc == NULL) {
		return;
	}
	for (; wr != NULL; wr = wr->next) {
		wr->qp_type.xrc.remote_srqn = rqpair->xrc->resp_srqn;
	}
}

static struct spdk_nvmf_rdma_poller *
nvmf_rdma_rgroup_poller(struct spdk_nvmf_rdma_poll_group *rgroup,
			struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_poller *poller;

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			return poller;
		}
	}
	return NULL;
}

static int
nvmf_rdma_xrc_ini_create(struct spdk_nvmf_rdma_xrc_session *session, uint32_t index,
			 struct spdk_nvmf_rdma_poller *poller)
{
	struct spdk_nvmf_rdma_transport *rtransport = session->rtransport;
	struct spdk_nvmf_rdma_device *device = session->device;
	struct spdk_nvmf_rdma_xrc_ini *ini = &session->ini[index];
	struct ibv_qp_init_attr_ex attr = {};
	struct ibv_qp_attr mod = {};

	attr.qp_type = IBV_QPT_XRC_SEND;
	attr.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr.pd = device->pd;
	attr.send_cq = poller->cq->cq;
	attr.recv_cq = poller->cq->cq;
	attr.qp_context = NULL;
	attr.cap.max_send_wr = spdk_min(rtransport->rdma_opts.xrc_ini_send_depth,
					(uint32_t)device->attr.max_qp_wr);
	attr.cap.max_send_sge = spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);

	ini->qp = ibv_create_qp_ex(device->context, &attr);
	if (ini->qp == NULL) {
		SPDK_ERRLOG("XRC: ibv_create_qp_ex(INI) failed: %s\n", spdk_strerror(errno));
		return -1;
	}

	mod.qp_state = IBV_QPS_INIT;
	mod.pkey_index = session->rtr.pkey_index;
	mod.port_num = session->port_num;
	mod.qp_access_flags = 0;
	if (ibv_modify_qp(ini->qp, &mod, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
			  IBV_QP_ACCESS_FLAGS)) {
		SPDK_ERRLOG("XRC: INI QP 0x%x -> INIT failed: %s\n", ini->qp->qp_num, spdk_strerror(errno));
		ibv_destroy_qp(ini->qp);
		ini->qp = NULL;
		return -1;
	}

	ini->rdma_qp.qp = ini->qp;
	ini->rdma_qp.stats = &poller->stat.qp_stats;
	ini->rdma_qp.shared_stats = true;
	ini->poller = poller;
	ini->session = session;
	ini->index = index;
	ini->sq.rdma_qp = &ini->rdma_qp;
	ini->sq.max_send_depth = attr.cap.max_send_wr;
	ini->sq.max_read_depth = session->max_read_depth;
	ini->sq.xrc_ini = ini;
	STAILQ_INIT(&ini->sq.pending_rdma_read_queue);
	STAILQ_INIT(&ini->sq.pending_rdma_write_queue);
	STAILQ_INIT(&ini->sq.pending_rdma_send_queue);
	return 0;
}

/* Connect INI QP -> host TGT QP. Called once per session, under xrc_lock. */
static int
nvmf_rdma_xrc_ini_connect(struct spdk_nvmf_rdma_xrc_ini *ini, uint32_t host_tgt_qpn)
{
	struct spdk_nvmf_rdma_xrc_session *session = ini->session;
	struct ibv_qp_attr a = {};

	a.qp_state = IBV_QPS_RTR;
	a.ah_attr = session->rtr.ah_attr;
	a.path_mtu = session->rtr.path_mtu;
	a.dest_qp_num = host_tgt_qpn;
	/* The INI QP receives nothing but ACKs and READ responses. */
	a.rq_psn = 0;
	if (ibv_modify_qp(ini->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
			  IBV_QP_DEST_QPN | IBV_QP_RQ_PSN)) {
		SPDK_ERRLOG("XRC: INI QP 0x%x -> RTR (host TGT 0x%x) failed: %s\n",
			    ini->qp->qp_num, host_tgt_qpn, spdk_strerror(errno));
		return -1;
	}

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTS;
	a.timeout = session->rts.timeout;
	a.retry_cnt = session->rts.retry_cnt;
	/* The host SRQ may run dry: retry RNR forever, as RC does (rnr_retry 7). */
	a.rnr_retry = 7;
	a.sq_psn = spdk_nvmf_rdma_xrc_psn(ini->qp->qp_num);
	a.max_rd_atomic = session->max_read_depth;
	if (ibv_modify_qp(ini->qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
		SPDK_ERRLOG("XRC: INI QP 0x%x -> RTS failed: %s\n", ini->qp->qp_num, spdk_strerror(errno));
		return -1;
	}
	ini->host_tgt_qpn = host_tgt_qpn;
	return 0;
}

static void
nvmf_rdma_xrc_session_free(struct spdk_nvmf_rdma_xrc_session *session)
{
	SPDK_NOTICELOG("XRC: session 0x%" PRIx64 " (%s) destroyed\n", session->id, session->hostnqn);
	free(session);
}

/* On the INI QP's owner thread. */
static void
nvmf_rdma_xrc_ini_destroy(struct spdk_nvmf_rdma_xrc_ini *ini)
{
	struct spdk_nvmf_rdma_xrc_session *session = ini->session;
	struct spdk_nvmf_rdma_sq *sq = &ini->sq, *tmp;
	bool last;

	if (ini->qp == NULL) {
		goto out;
	}
	/* Drop it from the poller's batch list before the QP goes away. */
	if (ini->poller != NULL) {
		STAILQ_FOREACH(tmp, &ini->poller->qpairs_pending_send, send_link) {
			if (tmp == sq) {
				struct ibv_send_wr *bad_wr = NULL;

				spdk_rdma_provider_qp_flush_send_wrs(sq->rdma_qp, &bad_wr);
				STAILQ_REMOVE(&ini->poller->qpairs_pending_send, sq, spdk_nvmf_rdma_sq, send_link);
				break;
			}
		}
	}
	if (sq->current_send_depth != 0 || sq->current_read_depth != 0 ||
	    !STAILQ_EMPTY(&sq->pending_rdma_read_queue) || !STAILQ_EMPTY(&sq->pending_rdma_write_queue) ||
	    !STAILQ_EMPTY(&sq->pending_rdma_send_queue)) {
		SPDK_ERRLOG("XRC: INI QP 0x%x (worker %u, session 0x%" PRIx64 ") destroyed with residue: "
			    "send_depth %u read_depth %u pending read/write/send %d/%d/%d\n",
			    ini->qp->qp_num, ini->index, session->id, sq->current_send_depth,
			    sq->current_read_depth, !STAILQ_EMPTY(&sq->pending_rdma_read_queue),
			    !STAILQ_EMPTY(&sq->pending_rdma_write_queue),
			    !STAILQ_EMPTY(&sq->pending_rdma_send_queue));
	}
	SPDK_NOTICELOG("XRC: INI QP 0x%x worker %u session 0x%" PRIx64 " destroyed: send_depth %u "
		       "read_depth %u, peak pending READ queue %u, READ stalls %" PRIu64 "\n",
		       ini->qp->qp_num, ini->index, session->id, sq->current_send_depth,
		       sq->current_read_depth, sq->pending_read_peak, sq->read_stalls);
	if (ibv_destroy_qp(ini->qp)) {
		SPDK_ERRLOG("XRC: ibv_destroy_qp(INI) failed: %s\n", spdk_strerror(errno));
	}
	ini->qp = NULL;
out:
	last = __atomic_sub_fetch(&session->ini_alive, 1, __ATOMIC_SEQ_CST) == 0;
	if (last) {
		nvmf_rdma_xrc_session_free(session);
	}
}

static void
_nvmf_rdma_xrc_ini_destroy(void *ctx)
{
	nvmf_rdma_xrc_ini_destroy(ctx);
}

/* Drop a real qpair's reference. The last one tears the INI QPs down, each on
 * its owner thread (they share that thread's CQ and batch list). */
static void
nvmf_rdma_xrc_session_put(struct spdk_nvmf_rdma_xrc_session *session)
{
	struct spdk_nvmf_rdma_device *device = session->device;
	struct spdk_nvmf_rdma_xrc_ini *ini;
	struct spdk_thread *thread;
	uint32_t i, n;

	pthread_mutex_lock(&device->xrc_lock);
	assert(session->refcnt > 0);
	if (--session->refcnt > 0) {
		pthread_mutex_unlock(&device->xrc_lock);
		return;
	}
	TAILQ_REMOVE(&device->xrc_sessions, session, link);
	n = session->num_ini;
	pthread_mutex_unlock(&device->xrc_lock);

	for (i = 0; i < n; i++) {
		ini = &session->ini[i];
		if (ini->poller == NULL || ini->qp == NULL) {
			nvmf_rdma_xrc_ini_destroy(ini);
			continue;
		}
		thread = ini->poller->group->group.group->thread;
		if (thread == spdk_get_thread()) {
			nvmf_rdma_xrc_ini_destroy(ini);
		} else {
			spdk_thread_send_msg(thread, _nvmf_rdma_xrc_ini_destroy, ini);
		}
	}
}

/*
 * CM REQ of an XRC I/O queue, on the acceptor thread: find the host session
 * or create it with one INI QP per poll group (not connected yet).
 */
static struct spdk_nvmf_rdma_xrc_session *
nvmf_rdma_xrc_session_get(struct spdk_nvmf_rdma_transport *rtransport,
			  struct spdk_nvmf_rdma_device *device, struct rdma_cm_event *event,
			  uint64_t session_id)
{
	struct spdk_nvmf_rdma_xrc_session *session;
	struct spdk_nvmf_rdma_poller *poller;
	int mask = 0;
	uint32_t i;

	pthread_mutex_lock(&device->xrc_lock);
	TAILQ_FOREACH(session, &device->xrc_sessions, link) {
		if (session->id == session_id) {
			if (session->state == NVMF_RDMA_XRC_SESSION_FAILED) {
				session = NULL;
			} else {
				session->refcnt++;
			}
			pthread_mutex_unlock(&device->xrc_lock);
			return session;
		}
	}

	session = calloc(1, sizeof(*session));
	if (session == NULL) {
		pthread_mutex_unlock(&device->xrc_lock);
		return NULL;
	}
	session->id = session_id;
	session->device = device;
	session->rtransport = rtransport;
	session->refcnt = 1;
	session->state = NVMF_RDMA_XRC_SESSION_CREATED;
	session->port_num = event->id->port_num;
	session->max_read_depth = spdk_min((uint32_t)device->attr.max_qp_init_rd_atom,
					   (uint32_t)event->param.conn.initiator_depth);

	session->rtr.qp_state = IBV_QPS_RTR;
	if (rdma_init_qp_attr(event->id, &session->rtr, &mask)) {
		SPDK_ERRLOG("XRC: rdma_init_qp_attr(RTR) failed: %s\n", spdk_strerror(errno));
		goto fail;
	}
	session->rts.qp_state = IBV_QPS_RTS;
	if (rdma_init_qp_attr(event->id, &session->rts, &mask)) {
		SPDK_ERRLOG("XRC: rdma_init_qp_attr(RTS) failed: %s\n", spdk_strerror(errno));
		goto fail;
	}

	for (i = 0; i < rtransport->xrc_num_pgs; i++) {
		poller = nvmf_rdma_rgroup_poller(rtransport->xrc_pgs[i], device);
		if (poller == NULL || poller->xrc_srq == NULL) {
			SPDK_ERRLOG("XRC: poll group %u has no XRC poller on %s\n", i,
				    ibv_get_device_name(device->context->device));
			goto fail;
		}
		if (nvmf_rdma_xrc_ini_create(session, i, poller)) {
			goto fail;
		}
		session->num_ini++;
	}
	session->ini_alive = session->num_ini;

	TAILQ_INSERT_TAIL(&device->xrc_sessions, session, link);
	pthread_mutex_unlock(&device->xrc_lock);
	SPDK_NOTICELOG("XRC: session 0x%" PRIx64 " created on %s: %u INI QPs, READ depth %u\n",
		       session_id, ibv_get_device_name(device->context->device), session->num_ini,
		       session->max_read_depth);
	return session;
fail:
	for (i = 0; i < session->num_ini; i++) {
		ibv_destroy_qp(session->ini[i].qp);
	}
	pthread_mutex_unlock(&device->xrc_lock);
	free(session);
	return NULL;
}

/*
 * First Fabrics Connect of a session, on the queue's home thread: connect
 * every INI QP to the host's TGT QP for that poll group. Later Connects only
 * check that the queue belongs to the same host.
 */
static int
nvmf_rdma_xrc_session_connect(struct spdk_nvmf_rdma_qpair *rqpair,
			      struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_xrc_session *session = rqpair->xrc->session;
	struct spdk_nvmf_rdma_device *device = session->device;
	struct spdk_nvme_sgl_descriptor *sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;
	struct spdk_nvmf_fabric_connect_data *data;
	struct spdk_nvmf_rdma_xrc_connect_ext ext;
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	int rc = 0;
	uint32_t i;

	if (sgl->generic.type != SPDK_NVME_SGL_TYPE_DATA_BLOCK ||
	    sgl->unkeyed.subtype != SPDK_NVME_SGL_SUBTYPE_OFFSET ||
	    sgl->unkeyed.length < sizeof(*data) ||
	    sgl->address + sizeof(*data) > rqpair->qpair.transport->opts.in_capsule_data_size) {
		SPDK_ERRLOG("XRC: Fabrics Connect data must be in the capsule (SGL type %u subtype %u "
			    "addr 0x%" PRIx64 " len %u)\n", sgl->generic.type, sgl->unkeyed.subtype,
			    sgl->address, sgl->unkeyed.length);
		return -EINVAL;
	}
	data = (struct spdk_nvmf_fabric_connect_data *)(rdma_req->recv->buf + sgl->address);
	memcpy(&ext, (uint8_t *)data + SPDK_NVMF_RDMA_XRC_CONNECT_EXT_OFFSET, sizeof(ext));
	snprintf(hostnqn, sizeof(hostnqn), "%.*s", SPDK_NVMF_NQN_MAX_LEN, (char *)data->hostnqn);

	if (ext.xrc_magic != SPDK_NVMF_RDMA_XRC_MAGIC || ext.session_id != session->id ||
	    ext.num_tgt != session->num_ini) {
		SPDK_ERRLOG("XRC: bad Connect extension (magic 0x%x session 0x%" PRIx64 " num_tgt %u, "
			    "expected session 0x%" PRIx64 " num_tgt %u)\n", ext.xrc_magic, ext.session_id,
			    ext.num_tgt, session->id, session->num_ini);
		return -EINVAL;
	}

	pthread_mutex_lock(&device->xrc_lock);
	switch (session->state) {
	case NVMF_RDMA_XRC_SESSION_CREATED:
		for (i = 0; i < session->num_ini; i++) {
			if (nvmf_rdma_xrc_ini_connect(&session->ini[i], ext.tgt_qpn[i])) {
				rc = -EIO;
				break;
			}
		}
		if (rc) {
			session->state = NVMF_RDMA_XRC_SESSION_FAILED;
			break;
		}
		memcpy(&session->hostid, data->hostid, sizeof(session->hostid));
		snprintf(session->hostnqn, sizeof(session->hostnqn), "%s", hostnqn);
		session->state = NVMF_RDMA_XRC_SESSION_CONNECTED;
		SPDK_NOTICELOG("XRC: session 0x%" PRIx64 " connected for host %s\n", session->id, hostnqn);
		break;
	case NVMF_RDMA_XRC_SESSION_CONNECTED:
		if (strcmp(session->hostnqn, hostnqn) != 0 ||
		    memcmp(&session->hostid, data->hostid, sizeof(session->hostid)) != 0) {
			SPDK_ERRLOG("XRC: queue of session 0x%" PRIx64 " connects as %s, session is %s\n",
				    session->id, hostnqn, session->hostnqn);
			rc = -EPERM;
			break;
		}
		for (i = 0; i < session->num_ini; i++) {
			if (session->ini[i].host_tgt_qpn != ext.tgt_qpn[i]) {
				SPDK_ERRLOG("XRC: session 0x%" PRIx64 " host TGT QPN %u changed\n", session->id, i);
				rc = -EINVAL;
				break;
			}
		}
		break;
	default:
		rc = -EIO;
		break;
	}
	pthread_mutex_unlock(&device->xrc_lock);
	return rc;
}

/* TGT QP of a real qpair, on its home thread before the CM accept. */
static int
nvmf_rdma_xrc_tgt_create(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_device *device = rqpair->device;
	struct spdk_nvmf_rdma_xrc_qpair *xrc = rqpair->xrc;
	struct ibv_qp_init_attr_ex attr = {};
	struct ibv_qp_attr cm = {}, a = {};
	int mask = 0;

	attr.qp_type = IBV_QPT_XRC_RECV;
	attr.comp_mask = IBV_QP_INIT_ATTR_XRCD;
	attr.xrcd = device->xrcd;
	xrc->tgt_qp = ibv_create_qp_ex(device->context, &attr);
	if (xrc->tgt_qp == NULL) {
		SPDK_ERRLOG("XRC: ibv_create_qp_ex(TGT) failed: %s\n", spdk_strerror(errno));
		return -1;
	}

	a.qp_state = IBV_QPS_INIT;
	a.pkey_index = xrc->session->rtr.pkey_index;
	a.port_num = rqpair->cm_id->port_num;
	a.qp_access_flags = 0;
	if (ibv_modify_qp(xrc->tgt_qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
			  IBV_QP_ACCESS_FLAGS)) {
		SPDK_ERRLOG("XRC: TGT QP -> INIT failed: %s\n", spdk_strerror(errno));
		goto fail;
	}

	cm.qp_state = IBV_QPS_RTR;
	if (rdma_init_qp_attr(rqpair->cm_id, &cm, &mask)) {
		SPDK_ERRLOG("XRC: rdma_init_qp_attr(RTR) failed: %s\n", spdk_strerror(errno));
		goto fail;
	}
	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTR;
	a.ah_attr = cm.ah_attr;
	a.path_mtu = cm.path_mtu;
	a.dest_qp_num = cm.dest_qp_num;
	a.rq_psn = cm.rq_psn;
	/* The host never RDMA-READs the target. */
	a.max_dest_rd_atomic = 0;
	a.min_rnr_timer = cm.min_rnr_timer;
	if (ibv_modify_qp(xrc->tgt_qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
			  IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		SPDK_ERRLOG("XRC: TGT QP 0x%x -> RTR failed: %s\n", xrc->tgt_qp->qp_num, spdk_strerror(errno));
		goto fail;
	}
	xrc->tgt_qpn = xrc->tgt_qp->qp_num;
	return 0;
fail:
	ibv_destroy_qp(xrc->tgt_qp);
	xrc->tgt_qp = NULL;
	return -1;
}

static int
nvmf_rdma_xrc_accept(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_xrc_qpair *xrc = rqpair->xrc;
	struct spdk_nvmf_rdma_xrc_session *session = xrc->session;
	uint8_t buf[sizeof(struct spdk_nvmf_rdma_xrc_accept_private_data) +
					   2 * sizeof(uint32_t) * SPDK_NVMF_RDMA_XRC_MAX_WORKERS] = {};
	struct spdk_nvmf_rdma_xrc_accept_private_data *pd = (void *)buf;
	struct spdk_nvmf_rdma_transport *rtransport = session->rtransport;
	struct spdk_nvmf_rdma_poller *poller;
	struct rdma_conn_param param = {};
	uint32_t i, n = session->num_ini;
	int rc;

	pd->recfmt = 0;
	pd->crqsize = rqpair->max_queue_depth;
	pd->xrc_magic = SPDK_NVMF_RDMA_XRC_MAGIC;
	pd->xrc_version = SPDK_NVMF_RDMA_XRC_VERSION;
	pd->num_workers = n;
	pd->home = xrc->worker;
	pd->tgt_qpn = xrc->tgt_qpn;
	for (i = 0; i < n; i++) {
		poller = nvmf_rdma_rgroup_poller(rtransport->xrc_pgs[i], rqpair->device);
		assert(poller != NULL);
		pd->data[i] = poller->xrc_srqn;
		pd->data[n + i] = session->ini[i].qp->qp_num;
	}

	param.private_data = pd;
	param.private_data_len = sizeof(*pd) + 2 * sizeof(uint32_t) * n;
	param.responder_resources = 0;
	param.initiator_depth = session->max_read_depth;
	param.rnr_retry_count = 0x7;
	param.srq = 1;
	param.qp_num = xrc->tgt_qpn;

	rc = rdma_accept(rqpair->cm_id, &param);
	if (rc) {
		SPDK_ERRLOG("XRC: rdma_accept failed: %s\n", spdk_strerror(errno));
	}
	return rc;
}

/* Worker of the poller a qpair lives on. */
static inline uint32_t
nvmf_rdma_poller_worker(struct spdk_nvmf_rdma_poller *rpoller)
{
	return rpoller->group->xrc_index;
}

/*
 * A command for TGT QP qp_num arrived on a poll group that has no qpair for it
 * yet: create the queue's shadow here, on this thread. NULL if the queue is
 * gone or closing (the command is then dropped and its buffer reposted).
 */
static struct spdk_nvmf_rdma_qpair *
nvmf_rdma_xrc_shadow_get(struct spdk_nvmf_rdma_poller *rpoller, uint32_t qp_num)
{
	struct spdk_nvmf_rdma_device *device = rpoller->device;
	struct spdk_nvmf_rdma_xrc_qpair find = {}, *real_xrc;
	struct spdk_nvmf_rdma_qpair *real, *shadow = NULL;
	struct spdk_nvmf_rdma_xrc_ini *ini;
	uint32_t w = nvmf_rdma_poller_worker(rpoller);

	find.tgt_qpn = qp_num;
	pthread_mutex_lock(&device->xrc_lock);
	real_xrc = RB_FIND(xrc_tgt_tree, &device->xrc_tgts, &find);
	if (real_xrc == NULL || real_xrc->closing || real_xrc->shadows[w] != NULL) {
		goto out;
	}
	real = real_xrc->rqpair;
	if (real->qpair.state != SPDK_NVMF_QPAIR_ENABLED || real->qpair.ctrlr == NULL ||
	    real_xrc->session->state != NVMF_RDMA_XRC_SESSION_CONNECTED) {
		goto out;
	}

	shadow = calloc(1, sizeof(*shadow));
	if (shadow == NULL) {
		goto out;
	}
	shadow->xrc = calloc(1, sizeof(*shadow->xrc));
	if (shadow->xrc == NULL) {
		free(shadow);
		shadow = NULL;
		goto out;
	}
	shadow->xrc->rqpair = shadow;
	shadow->xrc->real = real;
	shadow->xrc->is_shadow = true;
	shadow->xrc->session = real_xrc->session;
	shadow->xrc->tgt_qpn = qp_num;
	shadow->xrc->resp_srqn = real_xrc->resp_srqn;
	shadow->xrc->worker = w;

	ini = &real_xrc->session->ini[w];
	shadow->device = device;
	shadow->poller = rpoller;
	shadow->srq = rpoller->xrc_srq;
	shadow->resources = rpoller->xrc_resources;
	shadow->qp_num = qp_num;
	shadow->max_queue_depth = real->max_queue_depth;
	shadow->max_send_sge = real->max_send_sge;
	shadow->sq = &ini->sq;
	shadow->listen_id = real->listen_id;
	shadow->qpair.transport = real->qpair.transport;
	shadow->qpair.qid = real->qpair.qid;
	shadow->qpair.ctrlr = real->qpair.ctrlr;
	shadow->qpair.group = rpoller->group->group.group;
	shadow->qpair.numa = real->qpair.numa;
	shadow->qpair.sq_head_max = real->qpair.sq_head_max;
	TAILQ_INIT(&shadow->qpair.outstanding);
	shadow->qpair.state = SPDK_NVMF_QPAIR_ENABLED;

	real_xrc->shadows[w] = shadow;
	real_xrc->num_shadows++;
out:
	pthread_mutex_unlock(&device->xrc_lock);
	if (shadow != NULL) {
		RB_INSERT(qpairs_tree, &rpoller->qpairs, shadow);
		rpoller->stat.xrc_shadows_created++;
		SPDK_DEBUGLOG(rdma, "XRC: shadow of qid %u (TGT 0x%x) on worker %u\n",
			      shadow->qpair.qid, qp_num, w);
	}
	return shadow;
}

static void _nvmf_rdma_xrc_shadow_try_destroy(void *ctx);

/* Schedule a destroy attempt on the shadow's thread; never frees inline, since
 * callers up the stack may still hold the shadow. */
static void
nvmf_rdma_xrc_shadow_schedule_destroy(struct spdk_nvmf_rdma_qpair *shadow)
{
	if (shadow->xrc->destroy_scheduled) {
		return;
	}
	shadow->xrc->destroy_scheduled = true;
	spdk_thread_send_msg(shadow->qpair.group->thread, _nvmf_rdma_xrc_shadow_try_destroy, shadow);
}

static void
nvmf_rdma_xrc_shadow_state_cb(void *cb_arg, int status)
{
	nvmf_rdma_xrc_shadow_schedule_destroy(cb_arg);
}

static void
_nvmf_rdma_xrc_real_shadows_gone(void *ctx)
{
	struct spdk_nvmf_rdma_qpair *real = ctx;
	spdk_nvmf_transport_qpair_fini_cb cb = real->xrc->fini_cb;

	if (cb != NULL) {
		real->xrc->fini_cb = NULL;
		cb(real->xrc->fini_cb_arg);
	}
	nvmf_rdma_destroy_drained_qpair(real);
}

/* Free a drained shadow. Forced (poller teardown) frees whatever is left. */
static void
nvmf_rdma_xrc_shadow_destroy(struct spdk_nvmf_rdma_qpair *shadow)
{
	struct spdk_nvmf_rdma_xrc_qpair *xrc = shadow->xrc;
	struct spdk_nvmf_rdma_qpair *real = xrc->real;
	struct spdk_nvmf_rdma_device *device = shadow->device;
	struct spdk_nvmf_rdma_recv *rdma_recv, *recv_tmp;
	struct spdk_nvmf_rdma_poller *rpoller = shadow->poller;
	struct ibv_recv_wr *bad_wr = NULL;
	struct spdk_thread *home;
	bool notify;

	/* Receives this shadow never got a request for. */
	STAILQ_FOREACH_SAFE(rdma_recv, &shadow->resources->incoming_queue, link, recv_tmp) {
		if (rdma_recv->qpair == shadow) {
			STAILQ_REMOVE(&shadow->resources->incoming_queue, rdma_recv, spdk_nvmf_rdma_recv, link);
			rdma_recv->wr.next = NULL;
			spdk_rdma_provider_srq_queue_recv_wrs(rpoller->xrc_srq, &rdma_recv->wr);
			rpoller->xrc_recvs_held--;
		}
	}
	spdk_rdma_provider_srq_flush_recv_wrs(rpoller->xrc_srq, &bad_wr);

	if (shadow->qd != 0 || !TAILQ_EMPTY(&shadow->qpair.outstanding)) {
		SPDK_ERRLOG("XRC: shadow of qid %u on worker %u destroyed with qd %u, outstanding %d\n",
			    shadow->qpair.qid, xrc->worker, shadow->qd,
			    !TAILQ_EMPTY(&shadow->qpair.outstanding));
	}
	RB_REMOVE(qpairs_tree, &rpoller->qpairs, shadow);
	rpoller->stat.xrc_shadows_destroyed++;

	pthread_mutex_lock(&device->xrc_lock);
	real->xrc->cmds_per_worker[xrc->worker] += xrc->cmds;
	assert(real->xrc->shadows[xrc->worker] == shadow);
	real->xrc->shadows[xrc->worker] = NULL;
	assert(real->xrc->num_shadows > 0);
	notify = --real->xrc->num_shadows == 0;
	home = real->xrc->home_thread;
	pthread_mutex_unlock(&device->xrc_lock);

	free(xrc);
	free(shadow);

	if (notify && home != NULL) {
		/* Messages from one thread arrive in order, so the real qpair cannot be
		 * gone before this one is handled: it waits for num_shadows == 0. */
		spdk_thread_send_msg(home, _nvmf_rdma_xrc_real_shadows_gone, real);
	}
}

static void
_nvmf_rdma_xrc_shadow_try_destroy(void *ctx)
{
	struct spdk_nvmf_rdma_qpair *shadow = ctx;

	shadow->xrc->destroy_scheduled = false;
	if (shadow->qpair.state != SPDK_NVMF_QPAIR_DEACTIVATING) {
		return;
	}
	if (shadow->qd != 0 || !TAILQ_EMPTY(&shadow->qpair.outstanding)) {
		return;
	}
	nvmf_rdma_xrc_shadow_destroy(shadow);
}

/* The real qpair is closing: deactivate the shadow on its own thread and
 * release whatever of it is still queued. In-flight requests finish as usual
 * and each completion retries the destroy. */
static void
_nvmf_rdma_xrc_shadow_drain(void *ctx)
{
	struct spdk_nvmf_rdma_qpair *shadow = ctx;
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(shadow->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_request *rdma_req, *tmp;
	struct spdk_nvmf_request *req, *req_tmp;
	struct spdk_nvmf_rdma_sq *sq = shadow->sq;
	struct spdk_nvmf_poll_group *group = shadow->qpair.group;
	uint32_t sid;

	shadow->to_close = true;
	shadow->qpair.state_cb = nvmf_rdma_xrc_shadow_state_cb;
	shadow->qpair.state_cb_arg = shadow;
	shadow->qpair.state = SPDK_NVMF_QPAIR_DEACTIVATING;

	/* Queued on the shared INI QP or waiting for a buffer: complete them now. */
	STAILQ_FOREACH_SAFE(rdma_req, &sq->pending_rdma_send_queue, state_link, tmp) {
		if (rdma_req->req.qpair == &shadow->qpair) {
			nvmf_rdma_request_process(rtransport, rdma_req);
		}
	}
	STAILQ_FOREACH_SAFE(rdma_req, &sq->pending_rdma_read_queue, state_link, tmp) {
		if (rdma_req->req.qpair == &shadow->qpair &&
		    rdma_req->state == RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING) {
			nvmf_rdma_request_process(rtransport, rdma_req);
		}
	}
	STAILQ_FOREACH_SAFE(rdma_req, &sq->pending_rdma_write_queue, state_link, tmp) {
		if (rdma_req->req.qpair == &shadow->qpair) {
			nvmf_rdma_request_process(rtransport, rdma_req);
		}
	}
	STAILQ_FOREACH_SAFE(req, &shadow->poller->group->group.pending_buf_queue, buf_link, req_tmp) {
		if (req->qpair == &shadow->qpair) {
			rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
			nvmf_rdma_request_process(rtransport, rdma_req);
		}
	}
	/* Held back by a paused subsystem or namespace (never started). */
	for (sid = 0; sid < group->num_sgroups; sid++) {
		TAILQ_FOREACH_SAFE(req, &group->sgroups[sid].queued, link, req_tmp) {
			if (req->qpair == &shadow->qpair) {
				TAILQ_REMOVE(&group->sgroups[sid].queued, req, link);
				nvmf_rdma_request_free(req);
			}
		}
	}
	nvmf_rdma_xrc_shadow_schedule_destroy(shadow);
}

/* Real qpair is being closed (home thread): no new shadows, drain the others.
 * Returns true if cb_fn is held back until they are gone. */
static bool
nvmf_rdma_xrc_real_close(struct spdk_nvmf_rdma_qpair *rqpair,
			 spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_xrc_qpair *xrc = rqpair->xrc;
	struct spdk_nvmf_rdma_device *device = rqpair->device;
	struct spdk_nvmf_rdma_qpair *shadows[SPDK_NVMF_RDMA_XRC_MAX_WORKERS];
	struct ibv_qp_attr a = {};
	uint32_t i, n = 0;

	if (xrc->tgt_qp != NULL) {
		/* Stop taking commands for this queue. */
		a.qp_state = IBV_QPS_ERR;
		ibv_modify_qp(xrc->tgt_qp, &a, IBV_QP_STATE);
	}

	pthread_mutex_lock(&device->xrc_lock);
	if (xrc->closing) {
		pthread_mutex_unlock(&device->xrc_lock);
		return false;
	}
	xrc->closing = true;
	for (i = 0; i < SPDK_NVMF_RDMA_XRC_MAX_WORKERS; i++) {
		if (xrc->shadows[i] != NULL) {
			shadows[n++] = xrc->shadows[i];
		}
	}
	if (n != 0) {
		xrc->fini_cb = cb_fn;
		xrc->fini_cb_arg = cb_arg;
	}
	pthread_mutex_unlock(&device->xrc_lock);

	/* A shadow is freed only on its own thread after this message, and a
	 * shadow cannot appear any more (closing). */
	for (i = 0; i < n; i++) {
		spdk_thread_send_msg(shadows[i]->qpair.group->thread, _nvmf_rdma_xrc_shadow_drain, shadows[i]);
	}
	return n != 0 && cb_fn != NULL;
}

static bool
nvmf_rdma_xrc_real_has_shadows(struct spdk_nvmf_rdma_qpair *rqpair)
{
	bool has;

	pthread_mutex_lock(&rqpair->device->xrc_lock);
	has = rqpair->xrc->num_shadows != 0;
	pthread_mutex_unlock(&rqpair->device->xrc_lock);
	return has;
}

/* Last part of nvmf_rdma_qpair_destroy() for a real XRC qpair. */
static void
nvmf_rdma_xrc_real_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_xrc_qpair *xrc = rqpair->xrc;
	struct spdk_nvmf_rdma_device *device = rqpair->device;
	char dist[SPDK_NVMF_RDMA_XRC_MAX_WORKERS * 22] = "";
	size_t off = 0;
	uint32_t i;

	pthread_mutex_lock(&device->xrc_lock);
	if (xrc->in_tree) {
		RB_REMOVE(xrc_tgt_tree, &device->xrc_tgts, xrc);
		xrc->in_tree = false;
	}
	xrc->closing = true;
	xrc->cmds_per_worker[xrc->worker] += xrc->cmds;
	pthread_mutex_unlock(&device->xrc_lock);

	if (xrc->num_shadows != 0) {
		SPDK_ERRLOG("XRC: qid %u destroyed with %u shadows left\n", rqpair->qpair.qid,
			    xrc->num_shadows);
	}
	if (xrc->session != NULL) {
		for (i = 0; i < xrc->session->num_ini && off < sizeof(dist); i++) {
			off += snprintf(dist + off, sizeof(dist) - off, "%s%" PRIu64, i ? " " : "",
					xrc->cmds_per_worker[i]);
		}
		SPDK_NOTICELOG("XRC: queue qid %u cntlid %u (TGT 0x%x, home %u, session 0x%" PRIx64
			       ") commands per worker: [%s]\n", rqpair->qpair.qid,
			       rqpair->qpair.ctrlr ? rqpair->qpair.ctrlr->cntlid : 0, xrc->tgt_qpn, xrc->worker,
			       xrc->session->id, dist);
	}
	if (xrc->tgt_qp != NULL) {
		if (ibv_destroy_qp(xrc->tgt_qp)) {
			SPDK_ERRLOG("XRC: ibv_destroy_qp(TGT) failed: %s\n", spdk_strerror(errno));
		}
		xrc->tgt_qp = NULL;
	}
	if (xrc->session != NULL) {
		nvmf_rdma_xrc_session_put(xrc->session);
		xrc->session = NULL;
	}
	free(xrc);
	rqpair->xrc = NULL;
}

/*
 * An INI QP failed: every queue of that host with a qpair on this poll group
 * loses its data/response path here, so all of them disconnect. On this
 * poller's thread.
 */
static void
nvmf_rdma_xrc_ini_failed(struct spdk_nvmf_rdma_poller *rpoller, struct spdk_nvmf_rdma_sq *sq)
{
	struct spdk_nvmf_rdma_xrc_ini *ini = sq->xrc_ini;
	struct spdk_nvmf_rdma_qpair *rqpair, *tmp;
	uint32_t n = 0;

	if (ini == NULL || ini->in_error) {
		return;
	}
	ini->in_error = true;
	RB_FOREACH_SAFE(rqpair, qpairs_tree, &rpoller->qpairs, tmp) {
		if (rqpair->sq == sq) {
			rqpair->ibv_in_error_state = true;
			nvmf_rdma_qpair_disconnect(rqpair);
			n++;
		}
	}
	SPDK_ERRLOG("XRC: INI QP 0x%x (worker %u, session 0x%" PRIx64 ", host %s) failed: "
		    "disconnecting the %u queues of that host on this worker\n", ini->qp->qp_num,
		    ini->index, ini->session->id, ini->session->hostnqn, n);
}

/* NVMe Abort for a queue whose commands may run on any poll group: visit
 * the poll groups one after the other until one holds the command. */
struct nvmf_rdma_xrc_abort_ctx {
	struct spdk_nvmf_request		*req;
	struct spdk_nvmf_rdma_qpair		*real;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_transport		*rtransport;
	uint32_t				tgt_qpn;
	uint32_t				next;
};

static void
_nvmf_rdma_xrc_abort_step(void *arg)
{
	struct nvmf_rdma_xrc_abort_ctx *ctx = arg;
	struct spdk_nvmf_rdma_transport *rtransport = ctx->rtransport;
	struct spdk_nvmf_request *req = ctx->req;
	struct spdk_nvmf_rdma_poller *rpoller;
	struct spdk_nvmf_rdma_qpair find = {}, *rqpair;
	struct spdk_nvmf_rdma_request *rdma_req;
	uint16_t cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	uint32_t i, idx = ctx->next++;

	rpoller = nvmf_rdma_rgroup_poller(rtransport->xrc_pgs[idx], ctx->device);
	if (rpoller != NULL && rpoller->xrc_resources != NULL) {
		find.qp_num = ctx->tgt_qpn;
		rqpair = RB_FIND(qpairs_tree, &rpoller->qpairs, &find);
		if (rqpair != NULL && rqpair->xrc != NULL && rqpair->xrc->real == ctx->real) {
			for (i = 0; i < rpoller->max_xrc_srq_depth; i++) {
				rdma_req = &rpoller->xrc_resources->reqs[i];
				if (rdma_req->state != RDMA_REQUEST_STATE_FREE &&
				    rdma_req->req.qpair == &rqpair->qpair &&
				    rdma_req->req.cmd->nvme_cmd.cid == cid) {
					SPDK_NOTICELOG("XRC: abort of qid %u cid %u found on worker %u (home %u)\n",
						       rqpair->qpair.qid, cid, idx, ctx->real->xrc ? ctx->real->xrc->worker : 0);
					free(ctx);
					req->req_to_abort = &rdma_req->req;
					req->timeout_tsc = spdk_get_ticks() +
							   rtransport->transport.opts.abort_timeout_sec * spdk_get_ticks_hz();
					req->poller = NULL;
					_nvmf_rdma_qpair_abort_request(req);
					return;
				}
			}
		}
	}

	if (ctx->next < rtransport->xrc_num_pgs) {
		spdk_thread_send_msg(rtransport->xrc_pgs[ctx->next]->group.group->thread,
				     _nvmf_rdma_xrc_abort_step, ctx);
		return;
	}
	free(ctx);
	/* Not found: cdw0 bit 0 stays 1 ("not aborted"). */
	spdk_nvmf_request_complete(req);
}

static void
nvmf_rdma_xrc_abort_request(struct spdk_nvmf_rdma_qpair *rqpair, struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct nvmf_rdma_xrc_abort_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL || rtransport->xrc_num_pgs == 0) {
		free(ctx);
		spdk_nvmf_request_complete(req);
		return;
	}
	ctx->req = req;
	ctx->real = rqpair;
	ctx->device = rqpair->device;
	ctx->rtransport = rtransport;
	ctx->tgt_qpn = rqpair->xrc->tgt_qpn;
	ctx->next = 0;
	spdk_thread_send_msg(rtransport->xrc_pgs[0]->group.group->thread, _nvmf_rdma_xrc_abort_step, ctx);
}

static void
nvmf_rdma_qpair_disconnect(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (spdk_likely(rqpair->xrc == NULL)) {
		spdk_nvmf_qpair_disconnect(&rqpair->qpair);
		return;
	}
	/* A queue's lifecycle belongs to its real qpair; a shadow follows it. */
	if (rqpair->xrc->is_shadow) {
		rqpair = rqpair->xrc->real;
	}
	spdk_nvmf_qpair_disconnect(&rqpair->qpair);
}

static bool nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_request *rdma_req);

static void _poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

static void _poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_poller *rpoller);

static void _nvmf_rdma_remove_destroyed_device(void *c);

static int nvmf_rdma_memory_domain_transfer_data(struct spdk_memory_domain *dst_domain,
		void *dst_domain_ctx,
		struct iovec *dst_iov, uint32_t dst_iovcnt,
		struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		struct iovec *src_iov, uint32_t src_iovcnt,
		struct spdk_memory_domain_translation_result *translation,
		spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg);

static inline enum spdk_nvme_media_error_status_code
nvmf_rdma_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;
	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return result;
}

/*
 * Return data_wrs to pool starting from \b data_wr
 * Request's own response and data WR are excluded
 */
static void
_nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			     struct ibv_send_wr *data_wr,
			     struct spdk_mempool *pool)
{
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];
	struct spdk_nvmf_rdma_request_data	*nvmf_data;
	struct ibv_send_wr			*next_send_wr;
	uint64_t				req_wrid = (uint64_t)&rdma_req->data_wr;
	uint32_t				num_wrs = 0;

	while (data_wr && data_wr->wr_id == req_wrid) {
		nvmf_data = SPDK_CONTAINEROF(data_wr, struct spdk_nvmf_rdma_request_data, wr);
		memset(nvmf_data->sgl, 0, sizeof(data_wr->sg_list[0]) * data_wr->num_sge);
		data_wr->num_sge = 0;
		next_send_wr = data_wr->next;
		if (data_wr != &rdma_req->data.wr) {
			data_wr->next = NULL;
			assert(num_wrs < SPDK_NVMF_MAX_SGL_ENTRIES);
			work_requests[num_wrs] = nvmf_data;
			num_wrs++;
		}
		data_wr = (!next_send_wr || next_send_wr == &rdma_req->rsp.wr) ? NULL : next_send_wr;
	}

	if (num_wrs) {
		spdk_mempool_put_bulk(pool, (void **) work_requests, num_wrs);
	}
}

static void
nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			    struct spdk_nvmf_rdma_transport *rtransport)
{
	rdma_req->num_outstanding_data_wr = 0;

	_nvmf_rdma_request_free_data(rdma_req, rdma_req->transfer_wr, rtransport->data_wr_pool);

	if (rdma_req->remaining_tranfer_in_wrs) {
		_nvmf_rdma_request_free_data(rdma_req, rdma_req->remaining_tranfer_in_wrs,
					     rtransport->data_wr_pool);
		rdma_req->remaining_tranfer_in_wrs = NULL;
	}

	rdma_req->data.wr.next = NULL;
	rdma_req->rsp.wr.next = NULL;
}

static void
nvmf_rdma_dump_request(struct spdk_nvmf_rdma_request *req)
{
	SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", req->req.data_from_pool);
	if (req->req.cmd) {
		SPDK_ERRLOG("\t\tRequest opcode: %d\n", req->req.cmd->nvmf_cmd.opcode);
	}
	if (req->recv) {
		SPDK_ERRLOG("\t\tRequest recv wr_id%lu\n", req->recv->wr.wr_id);
	}
}

static void
nvmf_rdma_dump_qpair_contents(struct spdk_nvmf_rdma_qpair *rqpair)
{
	int i;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", rqpair->qpair.qid);
	for (i = 0; i < rqpair->max_queue_depth; i++) {
		if (rqpair->resources->reqs[i].state != RDMA_REQUEST_STATE_FREE) {
			nvmf_rdma_dump_request(&rqpair->resources->reqs[i]);
		}
	}
}

static void
nvmf_rdma_resources_destroy(struct spdk_nvmf_rdma_resources *resources)
{
	spdk_free(resources->cmds);
	spdk_free(resources->cpls);
	spdk_free(resources->bufs);
	spdk_free(resources->reqs);
	spdk_free(resources->recvs);
	free(resources);
}


static struct spdk_nvmf_rdma_resources *
nvmf_rdma_resources_create(struct spdk_nvmf_rdma_resource_opts *opts)
{
	struct spdk_nvmf_rdma_resources		*resources;
	struct spdk_nvmf_rdma_request		*rdma_req;
	struct spdk_nvmf_rdma_recv		*rdma_recv;
	struct spdk_rdma_provider_qp		*qp = NULL;
	struct spdk_rdma_provider_srq		*srq = NULL;
	struct ibv_recv_wr			*bad_wr = NULL;
	struct spdk_rdma_utils_memory_translation translation;
	uint32_t				i;
	int					rc = 0;

	resources = calloc(1, sizeof(struct spdk_nvmf_rdma_resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
		return NULL;
	}

	resources->reqs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->recvs = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->recvs),
					0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cmds = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cmds),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cpls = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (opts->in_capsule_data_size > 0) {
		resources->bufs = spdk_zmalloc(opts->max_queue_depth * opts->in_capsule_data_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	}

	if (!resources->reqs || !resources->recvs || !resources->cmds ||
	    !resources->cpls || (opts->in_capsule_data_size && !resources->bufs)) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		goto cleanup;
	}

	SPDK_DEBUGLOG(rdma, "Command Array: %p Length: %lx\n",
		      resources->cmds, opts->max_queue_depth * sizeof(*resources->cmds));
	SPDK_DEBUGLOG(rdma, "Completion Array: %p Length: %lx\n",
		      resources->cpls, opts->max_queue_depth * sizeof(*resources->cpls));
	if (resources->bufs) {
		SPDK_DEBUGLOG(rdma, "In Capsule Data Array: %p Length: %x\n",
			      resources->bufs, opts->max_queue_depth *
			      opts->in_capsule_data_size);
	}

	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	if (opts->shared) {
		srq = (struct spdk_rdma_provider_srq *)opts->qp;
	} else {
		qp = (struct spdk_rdma_provider_qp *)opts->qp;
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_recv = &resources->recvs[i];
		rdma_recv->qpair = opts->qpair;

		/* Set up memory to receive commands */
		if (resources->bufs) {
			rdma_recv->buf = (void *)((uintptr_t)resources->bufs + (i *
						  opts->in_capsule_data_size));
		}

		rdma_recv->rdma_wr.type = RDMA_WR_TYPE_RECV;

		rdma_recv->sgl[0].addr = (uintptr_t)&resources->cmds[i];
		rdma_recv->sgl[0].length = sizeof(resources->cmds[i]);
		rc = spdk_rdma_utils_get_translation(opts->map, &resources->cmds[i], sizeof(resources->cmds[i]),
						     &translation);
		if (rc) {
			goto cleanup;
		}
		rdma_recv->sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
		rdma_recv->wr.num_sge = 1;

		if (rdma_recv->buf) {
			rdma_recv->sgl[1].addr = (uintptr_t)rdma_recv->buf;
			rdma_recv->sgl[1].length = opts->in_capsule_data_size;
			rc = spdk_rdma_utils_get_translation(opts->map, rdma_recv->buf, opts->in_capsule_data_size,
							     &translation);
			if (rc) {
				goto cleanup;
			}
			rdma_recv->sgl[1].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
			rdma_recv->wr.num_sge++;
		}

		rdma_recv->wr.wr_id = (uintptr_t)&rdma_recv->rdma_wr;
		rdma_recv->wr.sg_list = rdma_recv->sgl;
		rdma_recv->srq = srq;
		if (srq) {
			spdk_rdma_provider_srq_queue_recv_wrs(srq, &rdma_recv->wr);
		} else {
			spdk_rdma_provider_qp_queue_recv_wrs(qp, &rdma_recv->wr);
		}
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_req = &resources->reqs[i];

		if (opts->qpair != NULL) {
			rdma_req->req.qpair = &opts->qpair->qpair;
		} else {
			rdma_req->req.qpair = NULL;
		}
		rdma_req->req.cmd = NULL;
		rdma_req->req.iovcnt = 0;
		rdma_req->req.stripped_data = NULL;

		/* Set up memory to send responses */
		rdma_req->req.rsp = &resources->cpls[i];

		rdma_req->rsp.sgl[0].addr = (uintptr_t)&resources->cpls[i];
		rdma_req->rsp.sgl[0].length = sizeof(resources->cpls[i]);
		rc = spdk_rdma_utils_get_translation(opts->map, &resources->cpls[i], sizeof(resources->cpls[i]),
						     &translation);
		if (rc) {
			goto cleanup;
		}
		rdma_req->rsp.sgl[0].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);

		rdma_req->rsp_wr.type = RDMA_WR_TYPE_SEND;
		rdma_req->rsp.wr.wr_id = (uintptr_t)&rdma_req->rsp_wr;
		rdma_req->rsp.wr.next = NULL;
		rdma_req->rsp.wr.opcode = IBV_WR_SEND;
		rdma_req->rsp.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->rsp.wr.sg_list = rdma_req->rsp.sgl;
		rdma_req->rsp.wr.num_sge = SPDK_COUNTOF(rdma_req->rsp.sgl);

		/* Set up memory for data buffers */
		rdma_req->data_wr.type = RDMA_WR_TYPE_DATA;
		rdma_req->data.wr.wr_id = (uintptr_t)&rdma_req->data_wr;
		rdma_req->data.wr.next = NULL;
		rdma_req->data.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->data.wr.sg_list = rdma_req->data.sgl;
		rdma_req->data.wr.num_sge = SPDK_COUNTOF(rdma_req->data.sgl);

		/* Initialize request state to FREE */
		rdma_req->state = RDMA_REQUEST_STATE_FREE;
		STAILQ_INSERT_TAIL(&resources->free_queue, rdma_req, state_link);
	}

	if (srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(srq, &bad_wr);
	} else {
		rc = spdk_rdma_provider_qp_flush_recv_wrs(qp, &bad_wr);
	}

	if (rc) {
		goto cleanup;
	}

	return resources;

cleanup:
	nvmf_rdma_resources_destroy(resources);
	return NULL;
}

static void
nvmf_rdma_qpair_clean_ibv_events(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;

	ctx = rqpair->last_wqe_reached_ctx;
	if (ctx) {
		ctx->rqpair = NULL;
		/* Memory allocated for ctx is freed in nvmf_rdma_qpair_process_last_wqe_event */
		rqpair->last_wqe_reached_ctx = NULL;
	}
}

static void nvmf_rdma_poller_destroy(struct spdk_nvmf_rdma_poller *poller);

static void
nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	int				rc;

	spdk_trace_record(TRACE_RDMA_QP_DESTROY, 0, 0, (uintptr_t)rqpair);

	if (rqpair->qd != 0) {
		struct spdk_nvmf_qpair *qpair = &rqpair->qpair;
		struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
				struct spdk_nvmf_rdma_transport, transport);
		struct spdk_nvmf_rdma_request *req;
		uint32_t i, max_req_count = 0;

		SPDK_WARNLOG("Destroying qpair when queue depth is %d\n", rqpair->qd);

		if (rqpair->srq == NULL) {
			nvmf_rdma_dump_qpair_contents(rqpair);
			max_req_count = rqpair->max_queue_depth;
		} else if (rqpair->poller && rqpair->resources) {
			max_req_count = nvmf_rdma_qpair_max_reqs(rqpair);
		}

		SPDK_DEBUGLOG(rdma, "Release incomplete requests\n");
		for (i = 0; i < max_req_count; i++) {
			req = &rqpair->resources->reqs[i];
			if (req->req.qpair == qpair && req->state != RDMA_REQUEST_STATE_FREE) {
				/* nvmf_rdma_request_process checks qpair ibv and internal state
				 * and completes a request */
				nvmf_rdma_request_process(rtransport, req);
			}
		}
		assert(rqpair->qd == 0);
	}

	if (rqpair->poller) {
		RB_REMOVE(qpairs_tree, &rqpair->poller->qpairs, rqpair);

		if (rqpair->srq != NULL && rqpair->resources != NULL) {
			/* Drop all received but unprocessed commands for this queue and return them to SRQ */
			STAILQ_FOREACH_SAFE(rdma_recv, &rqpair->resources->incoming_queue, link, recv_tmp) {
				if (rqpair == rdma_recv->qpair) {
					STAILQ_REMOVE(&rqpair->resources->incoming_queue, rdma_recv, spdk_nvmf_rdma_recv, link);
					if (rqpair->xrc != NULL) {
						rqpair->poller->xrc_recvs_held--;
						rdma_recv->wr.next = NULL;
					}
					spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
					if (rc) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
					}
				}
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp != NULL) {
			spdk_rdma_provider_qp_destroy(rqpair->rdma_qp);
			rqpair->rdma_qp = NULL;
		}

		if (rqpair->poller != NULL && rqpair->srq == NULL) {
			rqpair->poller->required_num_wr -= MAX_WR_PER_QP(rqpair->max_queue_depth);
		}
	}

	if (rqpair->srq == NULL && rqpair->resources != NULL) {
		nvmf_rdma_resources_destroy(rqpair->resources);
	}

	if (rqpair->xrc != NULL) {
		nvmf_rdma_xrc_real_destroy(rqpair);
	}

	nvmf_rdma_qpair_clean_ibv_events(rqpair);

	if (rqpair->destruct_channel) {
		spdk_put_io_channel(rqpair->destruct_channel);
		rqpair->destruct_channel = NULL;
	}

	if (rqpair->poller && rqpair->poller->need_destroy && RB_EMPTY(&rqpair->poller->qpairs)) {
		nvmf_rdma_poller_destroy(rqpair->poller);
	}

	/* destroy cm_id last so cma device will not be freed before we destroy the cq. */
	if (rqpair->cm_id) {
		rdma_destroy_id(rqpair->cm_id);
	}

	free(rqpair);
}

static int
nvmf_rdma_resize_cq(struct spdk_nvmf_rdma_qpair *rqpair, struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_poller	*rpoller;
	int				rc, num_cqe, required_num_wr;

	/* Enlarge CQ size dynamically */
	rpoller = rqpair->poller;
	required_num_wr = rpoller->required_num_wr + MAX_WR_PER_QP(rqpair->max_queue_depth);
	num_cqe = rpoller->num_cqe;
	if (num_cqe < required_num_wr) {
		num_cqe = spdk_max(num_cqe * 2, required_num_wr);
		num_cqe = spdk_min(num_cqe, device->attr.max_cqe);
	}

	if (rpoller->num_cqe != num_cqe) {
		if (device->context->device->transport_type == IBV_TRANSPORT_IWARP) {
			SPDK_ERRLOG("iWARP doesn't support CQ resize. Current capacity %u, required %u\n"
				    "Using CQ of insufficient size may lead to CQ overrun\n", rpoller->num_cqe, num_cqe);
			return -1;
		}
		if (required_num_wr > device->attr.max_cqe) {
			SPDK_ERRLOG("RDMA CQE requirement (%d) exceeds device max_cqe limitation (%d)\n",
				    required_num_wr, device->attr.max_cqe);
			return -1;
		}

		SPDK_DEBUGLOG(rdma, "Resize RDMA CQ from %d to %d\n", rpoller->num_cqe, num_cqe);
		rc = spdk_rdma_provider_cq_resize(rpoller->cq, num_cqe);
		if (rc) {
			SPDK_ERRLOG("RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		rpoller->num_cqe = num_cqe;
	}

	rpoller->required_num_wr = required_num_wr;
	return 0;
}

static int
nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_rdma_resource_opts	opts;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_rdma_provider_qp_init_attr	qp_init_attr = {};

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;

	if (rqpair->xrc != NULL) {
		struct spdk_nvmf_rdma_xrc_qpair *xrc = rqpair->xrc;

		xrc->worker = rqpair->poller->group->xrc_index;
		xrc->home_thread = spdk_get_thread();
		rqpair->srq = rqpair->poller->xrc_srq;
		if (rqpair->srq == NULL || nvmf_rdma_xrc_tgt_create(rqpair)) {
			goto error;
		}
		rqpair->qp_num = xrc->tgt_qpn;
		rqpair->resources = rqpair->poller->xrc_resources;
		rqpair->sq = &xrc->session->ini[xrc->worker].sq;
		rqpair->max_send_sge = spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
		rqpair->current_recv_depth = 0;
		rqpair->qpair.queue_depth = 0;

		pthread_mutex_lock(&device->xrc_lock);
		RB_INSERT(xrc_tgt_tree, &device->xrc_tgts, xrc);
		xrc->in_tree = true;
		pthread_mutex_unlock(&device->xrc_lock);
		SPDK_DEBUGLOG(rdma, "XRC queue qid %u: TGT QP 0x%x on worker %u\n", rqpair->qpair.qid,
			      xrc->tgt_qpn, xrc->worker);
		return 0;
	}

	qp_init_attr.qp_context	= rqpair;
	qp_init_attr.pd		= device->pd;
	qp_init_attr.cq		= rqpair->poller->cq;

	if (rqpair->srq) {
		qp_init_attr.srq		= rqpair->srq;
	} else {
		qp_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth;
	}

	/* SEND, READ, and WRITE operations */
	qp_init_attr.cap.max_send_wr	= (uint32_t)rqpair->max_queue_depth * 2;
	qp_init_attr.cap.max_send_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
	qp_init_attr.cap.max_recv_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
	qp_init_attr.stats		= &rqpair->poller->stat.qp_stats;
	qp_init_attr.domain_transfer	= rqpair->start_accel_sequence ?
					  nvmf_rdma_memory_domain_transfer_data : NULL;

	if (rqpair->srq == NULL && nvmf_rdma_resize_cq(rqpair, device) < 0) {
		SPDK_ERRLOG("Failed to resize the completion queue. Cannot initialize qpair.\n");
		goto error;
	}

	rqpair->rdma_qp = spdk_rdma_provider_qp_create(rqpair->cm_id, &qp_init_attr);
	if (!rqpair->rdma_qp) {
		goto error;
	}

	rqpair->qp_num = rqpair->rdma_qp->qp->qp_num;
	rqpair->sq->rdma_qp = rqpair->rdma_qp;

	rqpair->sq->max_send_depth = spdk_min((uint32_t)(rqpair->max_queue_depth * 2),
					  qp_init_attr.cap.max_send_wr);
	rqpair->max_send_sge = spdk_min(NVMF_DEFAULT_TX_SGE, qp_init_attr.cap.max_send_sge);
	spdk_trace_record(TRACE_RDMA_QP_CREATE, 0, 0, (uintptr_t)rqpair);
	SPDK_DEBUGLOG(rdma, "New RDMA Connection: %p\n", qpair);

	if (rqpair->poller->srq == NULL) {
		rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
		transport = &rtransport->transport;

		opts.qp = rqpair->rdma_qp;
		opts.map = device->map;
		opts.qpair = rqpair;
		opts.shared = false;
		opts.max_queue_depth = rqpair->max_queue_depth;
		opts.in_capsule_data_size = transport->opts.in_capsule_data_size;

		rqpair->resources = nvmf_rdma_resources_create(&opts);

		if (!rqpair->resources) {
			SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
			rdma_destroy_qp(rqpair->cm_id);
			goto error;
		}
	} else {
		rqpair->resources = rqpair->poller->resources;
	}

	rqpair->current_recv_depth = 0;
	STAILQ_INIT(&rqpair->sq->pending_rdma_read_queue);
	STAILQ_INIT(&rqpair->sq->pending_rdma_write_queue);
	STAILQ_INIT(&rqpair->sq->pending_rdma_send_queue);
	rqpair->qpair.queue_depth = 0;

	return 0;

error:
	rdma_destroy_id(rqpair->cm_id);
	rqpair->cm_id = NULL;
	return -1;
}

/* Append the given recv wr structure to the resource structs outstanding recvs list. */
/* This function accepts either a single wr or the first wr in a linked list. */
static void
nvmf_rdma_qpair_queue_recv_wrs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *first)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	if (rqpair->srq != NULL) {
		if (rqpair->xrc != NULL) {
			rqpair->poller->xrc_recvs_held--;
		}
		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, first);
	} else {
		if (spdk_rdma_provider_qp_queue_recv_wrs(rqpair->rdma_qp, first)) {
			STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_recv, rqpair, recv_link);
		}
	}

	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_recvs(rtransport, rqpair->poller);
	}
}

static inline void
request_transfer_in(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;

	qpair = req->qpair;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);

	nvmf_rdma_xrc_stamp(rqpair, rdma_req->transfer_wr);
	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->sq->rdma_qp, rdma_req->transfer_wr)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair->sq, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	assert(rqpair->sq->current_read_depth + rdma_req->num_outstanding_data_wr <= rqpair->sq->max_read_depth);
	rqpair->sq->current_read_depth += rdma_req->num_outstanding_data_wr;
	assert(rqpair->sq->current_send_depth + rdma_req->num_outstanding_data_wr <= rqpair->sq->max_send_depth);
	rqpair->sq->current_send_depth += rdma_req->num_outstanding_data_wr;
}

static inline void
nvmf_rdma_request_reset_transfer_in(struct spdk_nvmf_rdma_request *rdma_req,
				    struct spdk_nvmf_rdma_transport *rtransport)
{
	/* Put completed WRs back to pool and move transfer_wr pointer */
	_nvmf_rdma_request_free_data(rdma_req, rdma_req->transfer_wr, rtransport->data_wr_pool);
	rdma_req->transfer_wr = rdma_req->remaining_tranfer_in_wrs;
	rdma_req->remaining_tranfer_in_wrs = NULL;
	rdma_req->num_outstanding_data_wr = rdma_req->num_remaining_data_wr;
	rdma_req->num_remaining_data_wr = 0;
}

static inline int
request_prepare_transfer_in_part(struct spdk_nvmf_request *req, uint32_t num_reads_available)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct ibv_send_wr		*wr;
	uint32_t i;

	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);
	assert(num_reads_available > 0);
	assert(rdma_req->num_outstanding_data_wr > num_reads_available);
	wr = rdma_req->transfer_wr;

	for (i = 0; i < num_reads_available - 1; i++) {
		wr = wr->next;
	}

	rdma_req->remaining_tranfer_in_wrs = wr->next;
	rdma_req->num_remaining_data_wr = rdma_req->num_outstanding_data_wr - num_reads_available;
	rdma_req->num_outstanding_data_wr = num_reads_available;
	/* Break chain of WRs to send only part. Once this portion completes, we continue sending RDMA_READs */
	wr->next = NULL;

	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req, int *data_posted)
{
	int				num_outstanding_data_wr = 0;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_send_wr		*first = NULL;
	struct spdk_nvmf_rdma_transport *rtransport;

	*data_posted = 0;
	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	if (spdk_unlikely(rqpair->xrc != NULL)) {
		/* Commands of this queue complete on several cores: no SQ head to
		 * report. The CQE's SQID lets the host find the queue, since all its
		 * queues on one poller share one receive SRQ. */
		rsp->sqhd = SPDK_NVMF_RDMA_XRC_SQHD;
		rsp->sqid = qpair->qid;
	} else {
		/* Advance our sq_head pointer */
		if (qpair->sq_head == qpair->sq_head_max) {
			qpair->sq_head = 0;
		} else {
			qpair->sq_head++;
		}
		rsp->sqhd = qpair->sq_head;
	}

	/* queue the capsule for the recv buffer */
	assert(rdma_req->recv != NULL);

	nvmf_rdma_qpair_queue_recv_wrs(rqpair, &rdma_req->recv->wr);

	rdma_req->recv = NULL;
	assert(rqpair->current_recv_depth > 0);
	rqpair->current_recv_depth--;

	/* Build the response which consists of optional
	 * RDMA WRITEs to transfer data, plus an RDMA SEND
	 * containing the response.
	 */
	first = &rdma_req->rsp.wr;

	if (spdk_unlikely(rsp->status.sc != SPDK_NVME_SC_SUCCESS)) {
		/* On failure, data was not read from the controller. So clear the
		 * number of outstanding data WRs to zero.
		 */
		rdma_req->num_outstanding_data_wr = 0;
	} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST && !rdma_req->data_transferred) {
		first = rdma_req->transfer_wr;
		*data_posted = 1;
		num_outstanding_data_wr = rdma_req->num_outstanding_data_wr;
	}
	nvmf_rdma_xrc_stamp(rqpair, first);
	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->sq->rdma_qp, first)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair->sq, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	SPDK_DEBUGLOG(rdma, "sent req %p, data_posted %d, outstanding_wrs %u\n", rdma_req, *data_posted,
		      rdma_req->num_outstanding_data_wr);

	/* +1 for the rsp wr */
	assert(rqpair->sq->current_send_depth + num_outstanding_data_wr + 1 <= rqpair->sq->max_send_depth);
	rqpair->sq->current_send_depth += num_outstanding_data_wr + 1;

	return 0;
}

static int
request_transfer_out_data(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_send_wr		*first = NULL;
	struct spdk_nvmf_rdma_transport *rtransport;

	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(req->qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);
	first = rdma_req->transfer_wr;

	nvmf_rdma_xrc_stamp(rqpair, first);
	if (spdk_rdma_provider_qp_queue_send_wrs(rqpair->sq->rdma_qp, first)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair->sq, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	SPDK_DEBUGLOG(rdma, "sent data for req %p, outstanding_wrs %u\n", rdma_req,
		      rdma_req->num_outstanding_data_wr);

	assert(rqpair->sq->current_send_depth + rdma_req->num_outstanding_data_wr <= rqpair->sq->max_send_depth);
	rqpair->sq->current_send_depth += rdma_req->num_outstanding_data_wr;

	return 0;
}

static int
nvmf_rdma_event_accept(struct rdma_cm_id *id, struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_accept_private_data	accept_data;
	struct rdma_conn_param				ctrlr_event_data = {};
	int						rc;

	accept_data.recfmt = 0;
	accept_data.crqsize = rqpair->max_queue_depth;

	ctrlr_event_data.private_data = &accept_data;
	ctrlr_event_data.private_data_len = sizeof(accept_data);
	if (id->ps == RDMA_PS_TCP) {
		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
		ctrlr_event_data.initiator_depth = rqpair->sq->max_read_depth;
	}

	/* Configure infinite retries for the initiator side qpair.
	 * We need to pass this value to the initiator to prevent the
	 * initiator side NIC from completing SEND requests back to the
	 * initiator with status rnr_retry_count_exceeded. */
	ctrlr_event_data.rnr_retry_count = 0x7;

	/* When qpair is created without use of rdma cm API, an additional
	 * information must be provided to initiator in the connection response:
	 * whether qpair is using SRQ and its qp_num
	 * Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	ctrlr_event_data.srq = rqpair->srq ? 1 : 0;
	ctrlr_event_data.qp_num = rqpair->qp_num;

	rc = spdk_rdma_provider_qp_accept(rqpair->rdma_qp, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error %d on spdk_rdma_provider_qp_accept\n", errno);
	} else {
		SPDK_DEBUGLOG(rdma, "Sent back the accept\n");
	}

	return rc;
}

static void
nvmf_rdma_event_reject(struct rdma_cm_id *id, enum spdk_nvmf_rdma_transport_error error)
{
	struct spdk_nvmf_rdma_reject_private_data	rej_data;

	rej_data.recfmt = 0;
	rej_data.sts = error;

	rdma_reject(id, &rej_data, sizeof(rej_data));
}

static int
nvmf_rdma_connect(struct spdk_nvmf_transport *transport, struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct spdk_nvmf_rdma_port	*port;
	struct rdma_conn_param		*rdma_param = NULL;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	uint16_t			max_queue_depth;
	uint16_t			max_read_depth;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	assert(event->id != NULL); /* Impossible. Can't even reject the connection. */
	assert(event->id->verbs != NULL); /* Impossible. No way to handle this. */

	rdma_param = &event->param.conn;
	if (rdma_param->private_data == NULL ||
	    rdma_param->private_data_len < sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_ERRLOG("connect request: no private data provided\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
		return -1;
	}

	private_data = rdma_param->private_data;
	if (private_data->recfmt != 0) {
		SPDK_ERRLOG("Received RDMA private data with RECFMT != 0\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT);
		return -1;
	}

	SPDK_DEBUGLOG(rdma, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      event->id->verbs->device->name, event->id->verbs->device->dev_name);

	port = event->listen_id->context;
	SPDK_DEBUGLOG(rdma, "Listen Id was %p with verbs %p. ListenAddr: %p\n",
		      event->listen_id, event->listen_id->verbs, port);

	/* Figure out the supported queue depth. This is a multi-step process
	 * that takes into account hardware maximums, host provided values,
	 * and our target's internal memory limits */

	SPDK_DEBUGLOG(rdma, "Calculating Queue Depth\n");

	/* Start with the maximum queue depth allowed by the target */
	max_queue_depth = rtransport->transport.opts.max_queue_depth;
	max_read_depth = rtransport->transport.opts.max_queue_depth;
	SPDK_DEBUGLOG(rdma, "Target Max Queue Depth: %d\n",
		      rtransport->transport.opts.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_read_depth = spdk_min(max_read_depth, port->device->attr.max_qp_init_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	/* from man3 rdma_get_cm_event
	 * responder_resources - Specifies the number of responder resources that is requested by the recipient.
	 * The responder_resources field must match the initiator depth specified by the remote node when running
	 * the rdma_connect and rdma_accept functions. */
	if (rdma_param->responder_resources != 0) {
		if (private_data->qid) {
			SPDK_DEBUGLOG(rdma, "Host (Initiator) is not allowed to use RDMA operations,"
				      " responder_resources must be 0 but set to %u\n",
				      rdma_param->responder_resources);
		} else {
			SPDK_WARNLOG("Host (Initiator) is not allowed to use RDMA operations,"
				     " responder_resources must be 0 but set to %u\n",
				     rdma_param->responder_resources);
		}
	}
	/* from man3 rdma_get_cm_event
	 * initiator_depth - Specifies the maximum number of outstanding RDMA read operations that the recipient holds.
	 * The initiator_depth field must match the responder resources specified by the remote node when running
	 * the rdma_connect and rdma_accept functions. */
	if (rdma_param->initiator_depth == 0) {
		SPDK_ERRLOG("Host (Initiator) doesn't support RDMA_READ or atomic operations\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_IRD);
		return -1;
	}
	max_read_depth = spdk_min(max_read_depth, rdma_param->initiator_depth);

	SPDK_DEBUGLOG(rdma, "Host Receive Queue Size: %d\n", private_data->hrqsize);
	SPDK_DEBUGLOG(rdma, "Host Send Queue Size: %d\n", private_data->hsqsize);
	max_queue_depth = spdk_min(max_queue_depth, private_data->hrqsize);
	max_queue_depth = spdk_min(max_queue_depth, private_data->hsqsize + 1);

	SPDK_DEBUGLOG(rdma, "Final Negotiated Queue Depth: %d R/W Depth: %d\n",
		      max_queue_depth, max_read_depth);

	rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		return -1;
	}

	rqpair->sq = &rqpair->own_sq;
	rqpair->device = port->device;
	rqpair->max_queue_depth = max_queue_depth;
	rqpair->sq->max_read_depth = max_read_depth;
	rqpair->cm_id = event->id;
	rqpair->listen_id = event->listen_id;
	rqpair->qpair.transport = transport;
	/* use qid from the private data to determine the qpair type
	   qid will be set to the appropriate value when the controller is created */
	rqpair->qpair.qid = private_data->qid;
	rqpair->qpair.numa.id_valid = 1;
	rqpair->qpair.numa.id = spdk_rdma_cm_id_get_numa_id(rqpair->cm_id);
	rqpair->start_accel_sequence = spdk_rdma_provider_accel_sequence_supported() &&
				       rqpair->qpair.qid != 0 && !rtransport->transport.opts.dif_insert_or_strip &&
				       rtransport->rdma_opts.in_capsule_data_disabled;

	SPDK_DEBUGLOG(rdma, "qpair %p id %u support accel %s\n", rqpair, rqpair->qpair.qid,
		      rqpair->start_accel_sequence ? "YES" : "NO");

	if (rdma_param->private_data_len >= sizeof(struct spdk_nvmf_rdma_xrc_request_private_data) &&
	    ((const struct spdk_nvmf_rdma_xrc_request_private_data *)private_data)->xrc_magic ==
	    SPDK_NVMF_RDMA_XRC_MAGIC) {
		const struct spdk_nvmf_rdma_xrc_request_private_data *xpd = (const void *)private_data;

		if (!rtransport->rdma_opts.xrc || xpd->qid == 0 ||
		    xpd->xrc_version != SPDK_NVMF_RDMA_XRC_VERSION || port->device->xrcd == NULL) {
			SPDK_ERRLOG("XRC connect request refused (target xrc %d, qid %u, version %u)\n",
				    rtransport->rdma_opts.xrc, xpd->qid, xpd->xrc_version);
			free(rqpair);
			nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
			return -1;
		}
		rqpair->xrc = calloc(1, sizeof(*rqpair->xrc));
		if (rqpair->xrc == NULL) {
			free(rqpair);
			nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
			return -1;
		}
		rqpair->xrc->rqpair = rqpair;
		rqpair->xrc->real = rqpair;
		rqpair->xrc->resp_srqn = xpd->resp_srqn;
		rqpair->xrc->session = nvmf_rdma_xrc_session_get(rtransport, port->device, event,
				       xpd->session_id);
		if (rqpair->xrc->session == NULL) {
			free(rqpair->xrc);
			free(rqpair);
			nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
			return -1;
		}
		/* No accel sequences on XRC queues. */
		rqpair->start_accel_sequence = false;
	}

	event->id->context = &rqpair->qpair;

	spdk_nvmf_tgt_new_qpair(transport->tgt, &rqpair->qpair);

	return 0;
}

static inline void
nvmf_rdma_setup_wr(struct ibv_send_wr *wr, struct ibv_send_wr *next,
		   enum spdk_nvme_data_transfer xfer)
{
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		wr->opcode = IBV_WR_RDMA_WRITE;
		wr->send_flags = 0;
		wr->next = next;
	} else if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		wr->opcode = IBV_WR_RDMA_READ;
		wr->send_flags = IBV_SEND_SIGNALED;
		wr->next = NULL;
	} else {
		assert(0);
	}
}

static int
nvmf_request_alloc_wrs(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_request *rdma_req,
		       uint32_t num_sgl_descriptors)
{
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];
	struct spdk_nvmf_rdma_request_data	*current_data_wr;
	uint32_t				i;

	if (spdk_unlikely(num_sgl_descriptors > SPDK_NVMF_MAX_SGL_ENTRIES)) {
		SPDK_ERRLOG("Requested too much entries (%u), the limit is %u\n",
			    num_sgl_descriptors, SPDK_NVMF_MAX_SGL_ENTRIES);
		return -EINVAL;
	}

	if (spdk_unlikely(spdk_mempool_get_bulk(rtransport->data_wr_pool, (void **)work_requests,
						num_sgl_descriptors))) {
		return -ENOMEM;
	}

	current_data_wr = &rdma_req->data;

	for (i = 0; i < num_sgl_descriptors; i++) {
		nvmf_rdma_setup_wr(&current_data_wr->wr, &work_requests[i]->wr, rdma_req->req.xfer);
		current_data_wr->wr.next = &work_requests[i]->wr;
		current_data_wr = work_requests[i];
		current_data_wr->wr.sg_list = current_data_wr->sgl;
		current_data_wr->wr.wr_id = rdma_req->data.wr.wr_id;
	}

	nvmf_rdma_setup_wr(&current_data_wr->wr, &rdma_req->rsp.wr, rdma_req->req.xfer);

	return 0;
}

static inline void
nvmf_rdma_setup_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;

	wr->wr.rdma.rkey = sgl->keyed.key;
	wr->wr.rdma.remote_addr = sgl->address;
	nvmf_rdma_setup_wr(wr, &rdma_req->rsp.wr, rdma_req->req.xfer);
}

static inline void
nvmf_rdma_update_remote_addr(struct spdk_nvmf_rdma_request *rdma_req, uint32_t num_wrs)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;
	uint32_t			i;
	int				j;
	uint64_t			remote_addr_offset = 0;

	for (i = 0; i < num_wrs; ++i) {
		wr->wr.rdma.rkey = sgl->keyed.key;
		wr->wr.rdma.remote_addr = sgl->address + remote_addr_offset;
		for (j = 0; j < wr->num_sge; ++j) {
			remote_addr_offset += wr->sg_list[j].length;
		}
		wr = wr->next;
	}
}

static int
nvmf_rdma_fill_wr_sgl(struct spdk_nvmf_rdma_device *device,
		      struct spdk_nvmf_rdma_request *rdma_req,
		      struct ibv_send_wr *wr,
		      uint32_t total_length)
{
	struct spdk_rdma_utils_memory_translation mem_translation;
	struct ibv_sge	*sg_ele;
	struct iovec *iov;
	uint32_t lkey, remaining;
	int rc;

	wr->num_sge = 0;

	while (total_length && wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES) {
		iov = &rdma_req->req.iov[rdma_req->iovpos];
		rc = spdk_rdma_utils_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return rc;
		}

		lkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);
		sg_ele = &wr->sg_list[wr->num_sge];
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		sg_ele->lkey = lkey;
		sg_ele->addr = (uintptr_t)iov->iov_base + rdma_req->offset;
		sg_ele->length = remaining;
		SPDK_DEBUGLOG(rdma, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele, sg_ele->addr,
			      sg_ele->length);
		rdma_req->offset += sg_ele->length;
		total_length -= sg_ele->length;
		wr->num_sge++;

		if (rdma_req->offset == iov->iov_len) {
			rdma_req->offset = 0;
			rdma_req->iovpos++;
		}
	}

	if (spdk_unlikely(total_length)) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;
}

static int
nvmf_rdma_fill_wr_sgl_with_dif(struct spdk_nvmf_rdma_device *device,
			       struct spdk_nvmf_rdma_request *rdma_req,
			       struct ibv_send_wr *wr,
			       uint32_t total_length,
			       uint32_t num_extra_wrs)
{
	struct spdk_rdma_utils_memory_translation mem_translation;
	struct spdk_dif_ctx *dif_ctx = &rdma_req->req.dif.dif_ctx;
	struct ibv_sge *sg_ele;
	struct iovec *iov;
	struct iovec *rdma_iov;
	uint32_t lkey, remaining;
	uint32_t remaining_data_block, data_block_size, md_size;
	uint32_t sge_len;
	int rc;

	data_block_size = dif_ctx->block_size - dif_ctx->md_size;

	if (spdk_likely(!rdma_req->req.stripped_data)) {
		rdma_iov = rdma_req->req.iov;
		remaining_data_block = data_block_size;
		md_size = dif_ctx->md_size;
	} else {
		rdma_iov = rdma_req->req.stripped_data->iov;
		total_length = total_length / dif_ctx->block_size * data_block_size;
		remaining_data_block = total_length;
		md_size = 0;
	}

	wr->num_sge = 0;

	while (total_length && (num_extra_wrs || wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES)) {
		iov = rdma_iov + rdma_req->iovpos;
		rc = spdk_rdma_utils_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return rc;
		}

		lkey = spdk_rdma_utils_memory_translation_get_lkey(&mem_translation);
		sg_ele = &wr->sg_list[wr->num_sge];
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		while (remaining) {
			if (wr->num_sge >= SPDK_NVMF_MAX_SGL_ENTRIES) {
				if (num_extra_wrs > 0 && wr->next) {
					wr = wr->next;
					wr->num_sge = 0;
					sg_ele = &wr->sg_list[wr->num_sge];
					num_extra_wrs--;
				} else {
					break;
				}
			}
			sg_ele->lkey = lkey;
			sg_ele->addr = (uintptr_t)((char *)iov->iov_base + rdma_req->offset);
			sge_len = spdk_min(remaining, remaining_data_block);
			sg_ele->length = sge_len;
			SPDK_DEBUGLOG(rdma, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele,
				      sg_ele->addr, sg_ele->length);
			remaining -= sge_len;
			remaining_data_block -= sge_len;
			rdma_req->offset += sge_len;
			total_length -= sge_len;

			sg_ele++;
			wr->num_sge++;

			if (remaining_data_block == 0) {
				/* skip metadata */
				rdma_req->offset += md_size;
				total_length -= md_size;
				/* Metadata that do not fit this IO buffer will be included in the next IO buffer */
				remaining -= spdk_min(remaining, md_size);
				remaining_data_block = data_block_size;
			}

			if (remaining == 0) {
				/* By subtracting the size of the last IOV from the offset, we ensure that we skip
				   the remaining metadata bits at the beginning of the next buffer */
				rdma_req->offset -= spdk_min(iov->iov_len, rdma_req->offset);
				rdma_req->iovpos++;
			}
		}
	}

	if (spdk_unlikely(total_length)) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;
}

static inline uint32_t
nvmf_rdma_calc_num_wrs(uint32_t length, uint32_t io_unit_size, uint32_t block_size)
{
	/* estimate the number of SG entries and WRs needed to process the request */
	uint32_t num_sge = 0;
	uint32_t i;
	uint32_t num_buffers = SPDK_CEIL_DIV(length, io_unit_size);

	for (i = 0; i < num_buffers && length > 0; i++) {
		uint32_t buffer_len = spdk_min(length, io_unit_size);
		uint32_t num_sge_in_block = SPDK_CEIL_DIV(buffer_len, block_size);

		if (num_sge_in_block * block_size > buffer_len) {
			++num_sge_in_block;
		}
		num_sge += num_sge_in_block;
		length -= buffer_len;
	}
	return SPDK_CEIL_DIV(num_sge, SPDK_NVMF_MAX_SGL_ENTRIES);
}

static int
nvmf_rdma_request_fill_iovs(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct ibv_send_wr			*wr = &rdma_req->data.wr;
	int					rc;
	uint32_t				num_wrs = 1;
	uint32_t				length;

	rqpair = SPDK_CONTAINEROF(req->qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	/* rdma wr specifics */
	nvmf_rdma_setup_request(rdma_req);

	length = req->length;
	if (spdk_unlikely(req->dif_enabled)) {
		req->dif.orig_length = length;
		length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
		req->dif.elba_length = length;
	}

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport,
					   length);
	rdma_req->iovpos = 0;
	if (req->use_accel_seq) {
		goto out;
	}
	if (spdk_unlikely(rc != 0)) {
		return rc;
	}

	assert(req->iovcnt <= rqpair->max_send_sge);

	/* When dif_insert_or_strip is true and the I/O data length is greater than one block,
	 * the stripped_buffers are got for DIF stripping. */
	if (spdk_unlikely(req->dif_enabled && (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST)
			  && (req->dif.elba_length > req->dif.dif_ctx.block_size))) {
		rc = nvmf_request_get_stripped_buffers(req, &rgroup->group,
						       &rtransport->transport, req->dif.orig_length);
		if (rc != 0) {
			SPDK_INFOLOG(rdma, "Get stripped buffers fail %d, fallback to req.iov.\n", rc);
		}
	}

	if (spdk_unlikely(req->dif_enabled)) {
		num_wrs = nvmf_rdma_calc_num_wrs(length, rtransport->transport.opts.io_unit_size,
						 req->dif.dif_ctx.block_size);
		if (num_wrs > 1) {
			rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_wrs - 1);
			if (spdk_unlikely(rc != 0)) {
				goto err_exit;
			}
		}

		rc = nvmf_rdma_fill_wr_sgl_with_dif(device, rdma_req, wr, length, num_wrs - 1);
		if (spdk_unlikely(rc != 0)) {
			goto err_exit;
		}

		if (num_wrs > 1) {
			nvmf_rdma_update_remote_addr(rdma_req, num_wrs);
		}
	} else {
		rc = nvmf_rdma_fill_wr_sgl(device, rdma_req, wr, length);
		if (spdk_unlikely(rc != 0)) {
			goto err_exit;
		}
	}

out:
	/* set the number of outstanding data WRs for this request. */
	rdma_req->num_outstanding_data_wr = num_wrs;

	return rc;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	req->iovcnt = 0;
	return rc;
}

static int
nvmf_rdma_request_fill_iovs_multi_sgl(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_device *device,
				      struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct ibv_send_wr			*current_wr;
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct spdk_nvme_sgl_descriptor		*inline_segment, *desc;
	uint32_t				num_sgl_descriptors;
	uint32_t				lengths[SPDK_NVMF_MAX_SGL_ENTRIES], total_length = 0;
	uint32_t				i;
	int					rc;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	inline_segment = &req->cmd->nvme_cmd.dptr.sgl1;
	assert(inline_segment->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	assert(inline_segment->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);

	num_sgl_descriptors = inline_segment->unkeyed.length / sizeof(struct spdk_nvme_sgl_descriptor);
	assert(num_sgl_descriptors <= SPDK_NVMF_MAX_SGL_ENTRIES);

	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		if (spdk_likely(!req->dif_enabled)) {
			lengths[i] = desc->keyed.length;
		} else {
			req->dif.orig_length += desc->keyed.length;
			lengths[i] = spdk_dif_get_length_with_md(desc->keyed.length, &req->dif.dif_ctx);
			req->dif.elba_length += lengths[i];
		}
		total_length += lengths[i];
		desc++;
	}

	if (spdk_unlikely(total_length > rtransport->transport.opts.max_io_size)) {
		SPDK_ERRLOG("Multi SGL length 0x%x exceeds max io size 0x%x\n",
			    total_length, rtransport->transport.opts.max_io_size);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return -EINVAL;
	}

	rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_sgl_descriptors - 1);
	if (spdk_unlikely(rc != 0)) {
		return -ENOMEM;
	}

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport, total_length);
	if (req->use_accel_seq) {
		rdma_req->iovpos = 0;
		req->length = total_length;
		goto out;
	}
	if (spdk_unlikely(rc != 0)) {
		nvmf_rdma_request_free_data(rdma_req, rtransport);
		return rc;
	}

	/* When dif_insert_or_strip is true and the I/O data length is greater than one block,
	 * the stripped_buffers are got for DIF stripping. */
	if (spdk_unlikely(req->dif_enabled && (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST)
			  && (req->dif.elba_length > req->dif.dif_ctx.block_size))) {
		rc = nvmf_request_get_stripped_buffers(req, &rgroup->group,
						       &rtransport->transport, req->dif.orig_length);
		if (spdk_unlikely(rc != 0)) {
			SPDK_INFOLOG(rdma, "Get stripped buffers fail %d, fallback to req.iov.\n", rc);
		}
	}

	/* The first WR must always be the embedded data WR. This is how we unwind them later. */
	current_wr = &rdma_req->data.wr;
	assert(current_wr != NULL);

	req->length = 0;
	rdma_req->iovpos = 0;
	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		/* The descriptors must be keyed data block descriptors with an address, not an offset. */
		if (spdk_unlikely(desc->generic.type != SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK ||
				  desc->keyed.subtype != SPDK_NVME_SGL_SUBTYPE_ADDRESS)) {
			rc = -EINVAL;
			goto err_exit;
		}

		if (spdk_likely(!req->dif_enabled)) {
			rc = nvmf_rdma_fill_wr_sgl(device, rdma_req, current_wr, lengths[i]);
		} else {
			rc = nvmf_rdma_fill_wr_sgl_with_dif(device, rdma_req, current_wr,
							    lengths[i], 0);
		}
		if (spdk_unlikely(rc != 0)) {
			rc = -ENOMEM;
			goto err_exit;
		}

		req->length += desc->keyed.length;
		current_wr->wr.rdma.rkey = desc->keyed.key;
		current_wr->wr.rdma.remote_addr = desc->address;
		current_wr = current_wr->next;
		desc++;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	/* Go back to the last descriptor in the list. */
	desc--;
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
		if (desc->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
			rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
			rdma_req->rsp.wr.imm_data = desc->keyed.key;
		}
	}
#endif

out:
	rdma_req->num_outstanding_data_wr = num_sgl_descriptors;

	return rc;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	return rc;
}

static int
nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;
	int					rc;
	uint32_t				length;

	rsp = &req->rsp->nvme_cpl;
	sgl = &req->cmd->nvme_cmd.dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {

		length = sgl->keyed.length;
		if (spdk_unlikely(length > rtransport->transport.opts.max_io_size)) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, rtransport->transport.opts.max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}
#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
		if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
			if (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
				rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
				rdma_req->rsp.wr.imm_data = sgl->keyed.key;
			}
		}
#endif

		/* fill request length and populate iovs */
		req->length = length;

		rc = nvmf_rdma_request_fill_iovs(rtransport, device, rdma_req);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EINVAL) {
				SPDK_ERRLOG("SGL length exceeds the max I/O size\n");
				rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				return -1;
			}
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(rdma, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}

		SPDK_DEBUGLOG(rdma, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = rtransport->transport.opts.in_capsule_data_size;

		assert(!req->use_accel_seq);
		SPDK_DEBUGLOG(nvmf, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, sgl->unkeyed.length);

		if (spdk_unlikely(offset > max_len)) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
			rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
			return -1;
		}
		max_len -= (uint32_t)offset;

		if (spdk_unlikely(sgl->unkeyed.length > max_len)) {
			SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
				    sgl->unkeyed.length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		rdma_req->num_outstanding_data_wr = 0;
		req->data_from_pool = false;
		req->length = sgl->unkeyed.length;

		req->iov[0].iov_base = rdma_req->recv->buf + offset;
		req->iov[0].iov_len = req->length;
		req->iovcnt = 1;

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {

		rc = nvmf_rdma_request_fill_iovs_multi_sgl(rtransport, device, rdma_req);
		if (spdk_unlikely(rc == -ENOMEM)) {
			SPDK_DEBUGLOG(rdma, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		} else if (spdk_unlikely(rc == -EINVAL)) {
			SPDK_ERRLOG("Multi SGL element request length exceeds the max I/O size\n");
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		SPDK_DEBUGLOG(rdma, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static void
_nvmf_rdma_request_free(struct spdk_nvmf_rdma_request *rdma_req,
			struct spdk_nvmf_rdma_transport	*rtransport)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;

	assert(rdma_req->transfer_cpl_cb == NULL);
	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	if (spdk_unlikely(rqpair->xrc != NULL && rdma_req->recv != NULL)) {
		/* Completed without a response (queue closing, error): give the
		 * command buffer back to the shared SRQ, or the SRQ shrinks. */
		struct ibv_recv_wr *bad_recv_wr = NULL;

		rdma_req->recv->wr.next = NULL;
		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_req->recv->wr);
		if (spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr)) {
			SPDK_ERRLOG("XRC: unable to re-post rx descriptor\n");
		}
		rqpair->poller->xrc_recvs_held--;
		if (rqpair->current_recv_depth > 0) {
			rqpair->current_recv_depth--;
		}
		rdma_req->recv = NULL;
	}
	if (rdma_req->req.data_from_pool) {
		rgroup = rqpair->poller->group;

		spdk_nvmf_request_free_buffers(&rdma_req->req, &rgroup->group, &rtransport->transport);
	}
	if (rdma_req->req.stripped_data) {
		nvmf_request_free_stripped_buffers(&rdma_req->req,
						   &rqpair->poller->group->group,
						   &rtransport->transport);
	}
	if (rdma_req->data_transfer_mkey) {
		spdk_rdma_provider_memory_key_put_ref(rdma_req->data_transfer_mkey);
		rdma_req->data_transfer_mkey = NULL;
	}
	if (rdma_req->req.bdev_io) {
		spdk_bdev_free_io(rdma_req->req.bdev_io);
		rdma_req->req.bdev_io = NULL;
	}
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	rdma_req->req.length = 0;
	rdma_req->req.iovcnt = 0;
	rdma_req->offset = 0;
	rdma_req->req.dif_enabled = false;
	rdma_req->fused_failed = false;
	rdma_req->data_transferred = false;
	rdma_req->req.use_accel_seq = false;
	rdma_req->transfer_wr = NULL;
	if (rdma_req->fused_pair) {
		/* This req was part of a valid fused pair, but failed before it got to
		 * READ_TO_EXECUTE state.  This means we need to fail the other request
		 * in the pair, because it is no longer part of a valid pair.  If the pair
		 * already reached READY_TO_EXECUTE state, we need to kick it.
		 */
		rdma_req->fused_pair->fused_failed = true;
		if (rdma_req->fused_pair->state == RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
			nvmf_rdma_request_process(rtransport, rdma_req->fused_pair);
		}
		rdma_req->fused_pair = NULL;
	}
	memset(&rdma_req->req.dif, 0, sizeof(rdma_req->req.dif));
	rqpair->qd--;

	STAILQ_INSERT_HEAD(&rqpair->resources->free_queue, rdma_req, state_link);
	rqpair->qpair.queue_depth--;
	rdma_req->state = RDMA_REQUEST_STATE_FREE;
}

static void
nvmf_rdma_check_fused_ordering(struct spdk_nvmf_rdma_transport *rtransport,
			       struct spdk_nvmf_rdma_qpair *rqpair,
			       struct spdk_nvmf_rdma_request *rdma_req)
{
	enum spdk_nvme_cmd_fuse last, next;

	last = rqpair->fused_first ? rqpair->fused_first->req.cmd->nvme_cmd.fuse : SPDK_NVME_CMD_FUSE_NONE;
	next = rdma_req->req.cmd->nvme_cmd.fuse;

	assert(last != SPDK_NVME_CMD_FUSE_SECOND);

	if (spdk_likely(last == SPDK_NVME_CMD_FUSE_NONE && next == SPDK_NVME_CMD_FUSE_NONE)) {
		return;
	}

	if (last == SPDK_NVME_CMD_FUSE_FIRST) {
		if (next == SPDK_NVME_CMD_FUSE_SECOND) {
			/* This is a valid pair of fused commands.  Point them at each other
			 * so they can be submitted consecutively once ready to be executed.
			 */
			rqpair->fused_first->fused_pair = rdma_req;
			rdma_req->fused_pair = rqpair->fused_first;
			rqpair->fused_first = NULL;
			return;
		} else {
			/* Mark the last req as failed since it wasn't followed by a SECOND. */
			rqpair->fused_first->fused_failed = true;

			/* If the last req is in READY_TO_EXECUTE state, then call
			 * nvmf_rdma_request_process(), otherwise nothing else will kick it.
			 */
			if (rqpair->fused_first->state == RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
				nvmf_rdma_request_process(rtransport, rqpair->fused_first);
			}

			rqpair->fused_first = NULL;
		}
	}

	if (next == SPDK_NVME_CMD_FUSE_FIRST) {
		/* Set rqpair->fused_first here so that we know to check that the next request
		 * is a SECOND (and to fail this one if it isn't).
		 */
		rqpair->fused_first = rdma_req;
	} else if (next == SPDK_NVME_CMD_FUSE_SECOND) {
		/* Mark this req failed since it ia SECOND and the last one was not a FIRST. */
		rdma_req->fused_failed = true;
	}
}

static void
nvmf_rdma_poll_group_insert_need_buffer_req(struct spdk_nvmf_rdma_poll_group *rgroup,
		struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request *r;

	/* CONNECT commands have a timeout, so we need to avoid a CONNECT command
	 * from getting buried behind a long list of other non-FABRIC requests
	 * waiting for a buffer. Note that even though the CONNECT command's data is
	 * in-capsule, the request still goes to this STAILQ.
	 */
	if (spdk_likely(rdma_req->req.cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC)) {
		/* This is the most likely case. */
		STAILQ_INSERT_TAIL(&rgroup->group.pending_buf_queue, &rdma_req->req, buf_link);
		return;
	} else {
		/* STAILQ doesn't have INSERT_BEFORE, so we need to either INSERT_HEAD
		 * or INSERT_AFTER. Put it after any other FABRIC commands that are
		 * already in the queue.
		 */
		r = STAILQ_FIRST(&rgroup->group.pending_buf_queue);
		if (r == NULL || r->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC) {
			STAILQ_INSERT_HEAD(&rgroup->group.pending_buf_queue, &rdma_req->req, buf_link);
			return;
		}
		while (true) {
			struct spdk_nvmf_request *next;

			next = STAILQ_NEXT(r, buf_link);
			if (next == NULL || next->cmd->nvme_cmd.opc != SPDK_NVME_OPC_FABRIC) {
				STAILQ_INSERT_AFTER(&rgroup->group.pending_buf_queue, r, &rdma_req->req, buf_link);
				return;
			}
			r = next;
		}
	}
}

static inline void
nvmf_rdma_req_finish_data_transfer(struct spdk_nvmf_rdma_request *rdma_req, int rc)
{
	spdk_memory_domain_data_cpl_cb cb = rdma_req->transfer_cpl_cb;

	SPDK_DEBUGLOG(rdma, "req %p, finish data transfer, rc %d\n", rdma_req, rc);
	rdma_req->transfer_cpl_cb = NULL;
	assert(cb);
	cb(rdma_req->transfer_cpl_cb_arg, rc);
}

static inline struct ibv_send_wr *
nvmf_rdma_update_sges_with_accel_key(struct spdk_nvmf_rdma_request *rdma_req, uint32_t lkey,
				     uint32_t send_flags)
{
	struct ibv_send_wr *wr, *result = NULL;

	wr = &rdma_req->data.wr;

	if (wr->next == NULL || wr->next == &rdma_req->rsp.wr) {
		wr->num_sge = 1;
		wr->sg_list[0].lkey = lkey;
		wr->sg_list[0].addr = 0;
		wr->sg_list[0].length = rdma_req->req.length;
		wr->send_flags |= send_flags;
		result = wr;
	} else {
		/* Data block descriptor or multi SGL case */
		struct spdk_nvme_sgl_descriptor *inline_segment, *desc;
		uint32_t num_sgl_descriptors, i, desc_len;
		uint64_t offset = 0;

		inline_segment = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;
		num_sgl_descriptors = inline_segment->unkeyed.length / sizeof(struct spdk_nvme_sgl_descriptor);
		desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;

		for (i = 0; i < num_sgl_descriptors; i++) {
			desc_len = desc->keyed.length;
			assert(wr);
			assert(wr != &rdma_req->rsp.wr);
			wr->num_sge = 1;
			wr->sg_list[0].lkey = lkey;
			wr->sg_list[0].addr = offset;
			wr->sg_list[0].length = desc_len;
			wr->wr.rdma.remote_addr = desc->address;
			wr->wr.rdma.rkey = desc->keyed.key;
			wr->send_flags |= send_flags;
			offset += desc_len;

			result = wr;
			wr = wr->next;
			desc++;
		}
	}

	assert(result);
	return result;
}

static int
nvmf_rdma_memory_domain_transfer_data(struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
				      struct iovec *dst_iov, uint32_t dst_iovcnt,
				      struct spdk_memory_domain *src_domain, void *src_domain_ctx,
				      struct iovec *src_iov, uint32_t src_iovcnt,
				      struct spdk_memory_domain_translation_result *translation,
				      spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg)
{
	struct spdk_nvmf_rdma_request *rdma_req = dst_domain_ctx;
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	assert(rdma_req->req.use_accel_seq);
	assert(rdma_req->state == RDMA_REQUEST_STATE_EXECUTING);
	if (spdk_unlikely(!src_domain ||
			  spdk_memory_domain_get_dma_device_type(src_domain) != SPDK_DMA_DEVICE_TYPE_RDMA)) {
		SPDK_ERRLOG("Unexpected source memory domain %p, type %d\n", src_domain,
			    src_domain ? (int)spdk_memory_domain_get_dma_device_type(src_domain) : -1);
		return -ENOTSUP;
	}
	if (spdk_unlikely(dst_iovcnt != 1 || !translation || translation->iov_count != 1)) {
		SPDK_ERRLOG("Unexpected iovcnt %u or missed translation, rdma_req %p\n", dst_iovcnt, rdma_req);
		return -ENOTSUP;
	}

	/* While the req was in the RDMA_REQUEST_STATE_NEED_BUFFER state, we allocated a buffer[s] and filled
	 * one or more Work Requests. Each WR may have several sge elements which may point to memory chunks
	 * with different Memory Keys.
	 * But once we registered UMR, all memory is represented as a virtually contig chunk with start address 0.
	 * Now we need to update all SGEs with new addresses and UMR */

	rdma_req->transfer_cpl_cb = cpl_cb;
	rdma_req->transfer_cpl_cb_arg = cpl_cb_arg;

	if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		nvmf_rdma_update_sges_with_accel_key(rdma_req, translation->rdma.lkey, 0);
		SPDK_DEBUGLOG(rdma, "req %p, lkey %u, transfer H2C\n", rdma_req, translation->rdma.lkey);
		/* Write IO: UMR is configured on iovs, start transfer from the host, offload is applied
		 * during RDMA_READ operation. Once transfer_in completes, bdev layer writes data to
		 * the media */
		assert(rdma_req->req.data_from_pool);
		NVMF_RDMA_SQ_READ_INSERT(rqpair->sq, rdma_req);
		rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
	} else {
		/* Read IO: The callback is called when data from the bdev is in local buffers. UMR
		 * is registered on these buffers. */
		if (translation->size >= offsetof(struct spdk_memory_domain_translation_result,
						  rdma.memory_key) + sizeof(translation->rdma.memory_key) &&
		    translation->rdma.memory_key != NULL) {
			nvmf_rdma_update_sges_with_accel_key(rdma_req, translation->rdma.lkey, 0);
			/* Mkey object is available. We can increment the reference counter and handle IO as usual */
			rdma_req->data_transfer_mkey = translation->rdma.memory_key;
			spdk_rdma_provider_memory_key_get_ref(rdma_req->data_transfer_mkey);
			nvmf_rdma_req_finish_data_transfer(rdma_req, 0);

			return 0;
		} else {
			struct ibv_send_wr *last = nvmf_rdma_update_sges_with_accel_key(rdma_req, translation->rdma.lkey,
						   IBV_SEND_SIGNALED);
			/* We need to write data buffers to the host, but
			 * without sending a response. The response will be sent when bdev finishes IO request -
			 * when RDMA_WRITE completes and we call the cpl_cb */
			last->next = NULL;
			SPDK_DEBUGLOG(rdma, "req %p, lkey %u, transfer C2H\n", rdma_req, translation->rdma.lkey);
			STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_write_queue, rdma_req, state_link);
			rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
		}
	}
	nvmf_rdma_request_process(rtransport, rdma_req);

	return 0;
}

static inline bool
nvmf_rdma_request_need_accel_sequence(struct spdk_nvmf_rdma_qpair *rqpair,
				      struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_subsystem *subsys;
	struct spdk_nvmf_ns *ns;
	uint8_t opc;

	if (!rqpair->start_accel_sequence) {
		return false;
	}
	opc = rdma_req->req.cmd->nvme_cmd.opc;
	if (spdk_unlikely(opc == SPDK_NVME_OPC_FABRIC || !rqpair->qpair.ctrlr ||
			  !rqpair->qpair.ctrlr->subsys)) {
		return false;
	}
	if (opc != SPDK_NVME_OPC_READ && opc != SPDK_NVME_OPC_WRITE) {
		return false;
	}
	subsys = rqpair->qpair.ctrlr->subsys;
	ns = _nvmf_subsystem_get_ns(subsys, rdma_req->req.cmd->nvme_cmd.nsid);
	if (spdk_unlikely(!ns)) {
		return false;
	}

	SPDK_DEBUGLOG(rdma, "rqpair %p id %u, req %p, accel_seq %s\n", rqpair, rqpair->qpair.qid, rdma_req,
		      ns->accel_sequence ? "YES" : "NO");

	return ns->accel_sequence;
}

static inline int
nvmf_rdma_request_append_copy_task(struct spdk_nvmf_rdma_qpair *rqpair,
				   struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_poll_group *rgroup;
	int rc;

	rgroup = rqpair->poller->group;
	SPDK_DEBUGLOG(rdma, "req %p, offset %"PRIu64" opc %d append copy, buf %p len %zu, count %u\n",
		      rdma_req,
		      from_le64(&rdma_req->req.cmd->nvme_cmd.cdw10),
		      rdma_req->req.cmd->nvme_cmd.opc, rdma_req->req.iov[0].iov_base, rdma_req->req.iov[0].iov_len,
		      rdma_req->req.iovcnt);

	/* Append a copy task and wait for the accel module to call transfer_cb
	 * In the callback we expect to get a UMR and:
	 * Read IO: unprocessed data from bdev is in the iovs, start transfer to the host using UMR,
	 * data will be processed by HW during RDMA_WRITE operation
	 * Write IO: UMR is configured on iovs, start transfer from the host, offload is applied
	 * during RDMA_READ operation. Once transfer_in completes, bdev layer writes data to
	 * the media */
	assert(rdma_req->req.accel_sequence == NULL);
	rc = spdk_accel_append_copy(&rdma_req->req.accel_sequence, rgroup->accel_ch,
				    rdma_req->req.iov, rdma_req->req.iovcnt,
				    rqpair->rdma_qp->domain, rdma_req,
				    rdma_req->req.iov, rdma_req->req.iovcnt,
				    rqpair->rdma_qp->domain, rdma_req,
				    NULL, NULL);
	if (spdk_unlikely(rc)) {
		struct spdk_nvme_cpl *rsp;

		if (rc == -ENOMEM) {
			rgroup->stat.pending_accel_task++;
			return rc;
		}

		rsp = &rdma_req->req.rsp->nvme_cpl;
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
		rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
		SPDK_DEBUGLOG(rdma, "req %p, accel task failed, rc %d\n", rdma_req, rc);
		return rc;
	}
	rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;

	return 0;
}

bool
nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			  struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvme_cpl		*rsp = &rdma_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	enum spdk_nvmf_rdma_request_state prev_state;
	bool				progress = false;
	int				data_posted;
	uint32_t			num_blocks, num_rdma_reads_available, qdepth;
	uint8_t				rsp_needed;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;
	rgroup = rqpair->poller->group;

	assert(rdma_req->state != RDMA_REQUEST_STATE_FREE);

	/* If the queue pair is in an error state, force the request to the completed state
	 * to release resources. */
	if (spdk_unlikely(rqpair->ibv_in_error_state || !spdk_nvmf_qpair_is_active(&rqpair->qpair))) {
		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			STAILQ_REMOVE(&rgroup->group.pending_buf_queue, &rdma_req->req, spdk_nvmf_request, buf_link);
			break;
		case RDMA_REQUEST_STATE_NEED_ACCEL_TASK:
			STAILQ_REMOVE(&rgroup->pending_accel_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			NVMF_RDMA_SQ_READ_REMOVE(rqpair->sq, rdma_req);
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			if (rdma_req->num_remaining_data_wr) {
				/* Partially sent request is still in the pending_rdma_read_queue,
				 * remove it before completing */
				rdma_req->num_remaining_data_wr = 0;
				NVMF_RDMA_SQ_READ_REMOVE(rqpair->sq, rdma_req);
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			STAILQ_REMOVE(&rqpair->sq->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			STAILQ_REMOVE(&rqpair->sq->pending_rdma_send_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
			break;
		default:
			break;
		}
		if (rdma_req->transfer_cpl_cb) {
			nvmf_rdma_req_finish_data_transfer(rdma_req, -EIO);
			/* Wait for completion callback from the controller */
			return 0;
		}
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	}

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = rdma_req->state;

		SPDK_DEBUGLOG(rdma, "Request %p entering state %d\n", rdma_req, prev_state);

		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair, rqpair->qpair.queue_depth);
			rdma_recv = rdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			rdma_req->req.cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
			memset(rdma_req->req.rsp, 0, sizeof(*rdma_req->req.rsp));
			rdma_req->transfer_wr = &rdma_req->data.wr;

			if (spdk_unlikely(rqpair->ibv_in_error_state || !spdk_nvmf_qpair_is_active(&rqpair->qpair))) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				break;
			}

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&rdma_req->req, &rdma_req->req.dif.dif_ctx))) {
				rdma_req->req.dif_enabled = true;
			}

			if (spdk_unlikely(rqpair->xrc != NULL)) {
				if (spdk_unlikely(rdma_req->req.cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE)) {
					/* The two halves of a fused pair may run on different
					 * cores here, so XRC queues refuse fused commands. */
					rsp->status.sct = SPDK_NVME_SCT_GENERIC;
					rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
					rsp->status.dnr = 1;
					STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					break;
				}
				if (spdk_unlikely(!rqpair->xrc->is_shadow &&
						  rdma_req->req.cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC &&
						  rdma_req->req.cmd->nvmf_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT &&
						  nvmf_rdma_xrc_session_connect(rqpair, rdma_req) != 0)) {
					rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
					nvmf_rdma_qpair_disconnect(rqpair);
					break;
				}
			} else {
				nvmf_rdma_check_fused_ordering(rtransport, rqpair, rdma_req);
			}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
			rdma_req->rsp.wr.opcode = IBV_WR_SEND;
			rdma_req->rsp.wr.imm_data = 0;
#endif

			/* The next state transition depends on the data transfer needs of this request. */
			rdma_req->req.xfer = spdk_nvmf_req_get_xfer(&rdma_req->req);

			if (spdk_unlikely(rdma_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				SPDK_DEBUGLOG(rdma, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", rdma_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_NONE) {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_NEED_BUFFER;
			nvmf_rdma_poll_group_insert_need_buffer_req(rgroup, rdma_req);
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEED_BUFFER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			assert(rdma_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (&rdma_req->req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}
			rdma_req->req.use_accel_seq = nvmf_rdma_request_need_accel_sequence(rqpair, rdma_req);

			/* Try to get a data buffer */
			rc = nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
			if (spdk_unlikely(rc < 0)) {
				STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);
				STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				break;
			}

			if (rdma_req->req.iovcnt == 0) {
				/* No buffers available. */
				rgroup->stat.pending_data_buffer++;
				break;
			}

			STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);

			if (rdma_req->req.use_accel_seq) {
				STAILQ_INSERT_TAIL(&rgroup->pending_accel_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_NEED_ACCEL_TASK;
				break;
			}

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
			    rdma_req->req.data_from_pool) {
				NVMF_RDMA_SQ_READ_INSERT(rqpair->sq, rdma_req);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case RDMA_REQUEST_STATE_NEED_ACCEL_TASK:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEED_ACCEL_TASK, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rgroup->pending_accel_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}
			if (spdk_unlikely(nvmf_rdma_request_append_copy_task(rqpair, rdma_req))) {
				break;
			}
			STAILQ_REMOVE_HEAD(&rgroup->pending_accel_queue, state_link);
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->sq->pending_rdma_read_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			assert(rqpair->sq->max_send_depth >= rqpair->sq->current_send_depth);
			qdepth = rqpair->sq->max_send_depth - rqpair->sq->current_send_depth;
			assert(rqpair->sq->max_read_depth >= rqpair->sq->current_read_depth);
			num_rdma_reads_available = rqpair->sq->max_read_depth - rqpair->sq->current_read_depth;
			if (rdma_req->num_outstanding_data_wr > qdepth ||
			    rdma_req->num_outstanding_data_wr > num_rdma_reads_available) {
				if (num_rdma_reads_available && qdepth) {
					/* Send as much as we can */
					request_prepare_transfer_in_part(&rdma_req->req, spdk_min(num_rdma_reads_available, qdepth));
				} else {
					/* We can only have so many WRs outstanding. we have to wait until some finish. */
					rqpair->poller->stat.pending_rdma_read++;
					rqpair->sq->read_stalls++;
					break;
				}
			}

			/* We have already verified that this request is the head of the queue. */
			if (rdma_req->num_remaining_data_wr == 0) {
				NVMF_RDMA_SQ_READ_REMOVE_HEAD(rqpair->sq);
			}

			request_transfer_in(&rdma_req->req);
			rdma_req->state = RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;

			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (spdk_unlikely(rdma_req->req.dif_enabled)) {
				if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
					/* generate DIF for write operation */
					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);
					assert(num_blocks > 0);

					rc = spdk_dif_generate(rdma_req->req.iov, rdma_req->req.iovcnt,
							       num_blocks, &rdma_req->req.dif.dif_ctx);
					if (rc != 0) {
						SPDK_ERRLOG("DIF generation failed\n");
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						nvmf_rdma_qpair_disconnect(rqpair);
						break;
					}
				}

				assert(rdma_req->req.dif.elba_length >= rdma_req->req.length);
				/* set extended length before IO operation */
				rdma_req->req.length = rdma_req->req.dif.elba_length;
			}

			if (rdma_req->req.cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
				if (rdma_req->fused_failed) {
					/* This request failed FUSED semantics.  Fail it immediately, without
					 * even sending it to the target layer.
					 */
					rsp->status.sct = SPDK_NVME_SCT_GENERIC;
					rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
					STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					break;
				}

				if (rdma_req->fused_pair == NULL ||
				    rdma_req->fused_pair->state != RDMA_REQUEST_STATE_READY_TO_EXECUTE) {
					/* This request is ready to execute, but either we don't know yet if it's
					 * valid - i.e. this is a FIRST but we haven't received the next
					 * request yet or the other request of this fused pair isn't ready to
					 * execute.  So break here and this request will get processed later either
					 * when the other request is ready or we find that this request isn't valid.
					 */
					break;
				}
			}

			/* If we get to this point, and this request is a fused command, we know that
			 * it is part of valid sequence (FIRST followed by a SECOND) and that both
			 * requests are READY_TO_EXECUTE. So call spdk_nvmf_request_exec() both on this
			 * request, and the other request of the fused pair, in the correct order.
			 * Also clear the ->fused_pair pointers on both requests, since after this point
			 * we no longer need to maintain the relationship between these two requests.
			 */
			if (rdma_req->req.cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_SECOND) {
				assert(rdma_req->fused_pair != NULL);
				assert(rdma_req->fused_pair->fused_pair != NULL);
				rdma_req->fused_pair->state = RDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&rdma_req->fused_pair->req);
				rdma_req->fused_pair->fused_pair = NULL;
				rdma_req->fused_pair = NULL;
			}
			rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(&rdma_req->req);
			if (rdma_req->req.cmd->nvme_cmd.fuse == SPDK_NVME_CMD_FUSE_FIRST) {
				assert(rdma_req->fused_pair != NULL);
				assert(rdma_req->fused_pair->fused_pair != NULL);
				rdma_req->fused_pair->state = RDMA_REQUEST_STATE_EXECUTING;
				spdk_nvmf_request_exec(&rdma_req->fused_pair->req);
				rdma_req->fused_pair->fused_pair = NULL;
				rdma_req->fused_pair = NULL;
			}
			break;
		case RDMA_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
			    rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_write_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
			} else {
				STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
				if (rdma_req->req.bdev_io) {
					spdk_bdev_free_io(rdma_req->req.bdev_io);
					rdma_req->req.bdev_io = NULL;
				}
			}
			if (spdk_unlikely(rdma_req->req.dif_enabled)) {
				/* restore the original length */
				rdma_req->req.length = rdma_req->req.dif.orig_length;

				if (rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
					struct spdk_dif_error error_blk;

					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);
					if (!rdma_req->req.stripped_data) {
						rc = spdk_dif_verify(rdma_req->req.iov, rdma_req->req.iovcnt, num_blocks,
								     &rdma_req->req.dif.dif_ctx, &error_blk);
					} else {
						rc = spdk_dif_verify_copy(rdma_req->req.stripped_data->iov,
									  rdma_req->req.stripped_data->iovcnt,
									  rdma_req->req.iov, rdma_req->req.iovcnt, num_blocks,
									  &rdma_req->req.dif.dif_ctx, &error_blk);
					}
					if (rc) {
						struct spdk_nvme_cpl *rsp = &rdma_req->req.rsp->nvme_cpl;

						SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n", error_blk.err_type,
							    error_blk.err_offset);
						rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
						rsp->status.sc = nvmf_rdma_dif_error_to_compl_status(error_blk.err_type);
						STAILQ_REMOVE(&rqpair->sq->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
						STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req, state_link);
						rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
					}
				}
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			rsp_needed = rdma_req->transfer_cpl_cb == NULL;

			if (rdma_req != STAILQ_FIRST(&rqpair->sq->pending_rdma_write_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			if ((rqpair->sq->current_send_depth + rdma_req->num_outstanding_data_wr + rsp_needed) >
			    rqpair->sq->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish */
				rqpair->poller->stat.pending_rdma_write++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->sq->pending_rdma_write_queue, state_link);

			/* The data transfer will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 * We verified that data + response fit into send queue, so we can go to the next state directly
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->sq->pending_rdma_send_queue)) {
				/* This request needs to wait in line to send the completion */
				break;
			}

			assert(rqpair->sq->current_send_depth <= rqpair->sq->max_send_depth);
			if (rqpair->sq->current_send_depth == rqpair->sq->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish */
				rqpair->poller->stat.pending_rdma_send++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->sq->pending_rdma_send_queue, state_link);

			/* The response sending will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			if (!rdma_req->transfer_cpl_cb) {
				rc = request_transfer_out(&rdma_req->req, &data_posted);
				assert(rc == 0); /* No good way to handle this currently */
				if (spdk_unlikely(rc)) {
					rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				} else {
					rdma_req->state = data_posted ? RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST :
							  RDMA_REQUEST_STATE_COMPLETING;
				}
			} else {
				SPDK_DEBUGLOG(rdma, "req %p, transfer data\n", rdma_req);
				rc = request_transfer_out_data(&rdma_req->req);
				assert(rc == 0); /* No good way to handle this currently */
				if (spdk_unlikely(rc)) {
					rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				} else {
					rdma_req->state = RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST;
				}
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair, rqpair->qpair.queue_depth);

			rqpair->poller->stat.request_latency += spdk_get_ticks() - rdma_req->receive_tsc;
			_nvmf_rdma_request_free(rdma_req, rtransport);
			break;
		case RDMA_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (rdma_req->state != prev_state) {
			progress = true;
		}
	} while (rdma_req->state != prev_state);

	return progress;
}

/* Public API callbacks begin here */

#define SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE (SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE UINT32_MAX
#define SPDK_NVMF_RDMA_DEFAULT_NO_SRQ false
#define SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG 100
#define SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING false
#define SPDK_NVMF_RDMA_DEFAULT_DATA_WR_POOL_SIZE 4095

static void
nvmf_rdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->msdbd =			NVMF_DEFAULT_MSDBD;
	opts->transport_specific =      NULL;
	opts->data_wr_pool_size	=	SPDK_NVMF_RDMA_DEFAULT_DATA_WR_POOL_SIZE;
}

static int nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
			     spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

static inline bool
nvmf_rdma_is_rxe_device(struct spdk_nvmf_rdma_device *device)
{
	return device->attr.vendor_id == SPDK_RDMA_PROVIDER_RXE_VENDOR_ID_OLD ||
	       device->attr.vendor_id == SPDK_RDMA_PROVIDER_RXE_VENDOR_ID_NEW;
}

static int nvmf_rdma_accept(void *ctx);
static bool nvmf_rdma_retry_listen_port(struct spdk_nvmf_rdma_transport *rtransport);
static void destroy_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
			      struct spdk_nvmf_rdma_device *device);

static int
create_ib_device(struct spdk_nvmf_rdma_transport *rtransport, struct ibv_context *context,
		 struct spdk_nvmf_rdma_device **new_device)
{
	struct spdk_nvmf_rdma_device	*device;
	int				rc = 0;

	device = calloc(1, sizeof(*device));
	if (!device) {
		SPDK_ERRLOG("Unable to allocate memory for RDMA devices.\n");
		return -ENOMEM;
	}
	device->context = context;
	rc = ibv_query_device(device->context, &device->attr);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		free(device);
		return rc;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) == 0) {
		SPDK_WARNLOG("The libibverbs on this system supports SEND_WITH_INVALIDATE,");
		SPDK_WARNLOG("but the device with vendor ID %u does not.\n", device->attr.vendor_id);
	}

	/**
	 * The vendor ID is assigned by the IEEE and an ID of 0 implies Soft-RoCE.
	 * The Soft-RoCE RXE driver does not currently support send with invalidate,
	 * but incorrectly reports that it does. There are changes making their way
	 * through the kernel now that will enable this feature. When they are merged,
	 * we can conditionally enable this feature.
	 *
	 * TODO: enable this for versions of the kernel rxe driver that support it.
	 */
	if (nvmf_rdma_is_rxe_device(device)) {
		device->attr.device_cap_flags &= ~(IBV_DEVICE_MEM_MGT_EXTENSIONS);
	}
#endif

	rc = spdk_fd_set_nonblock(device->context->async_fd);
	if (rc < 0) {
		free(device);
		return rc;
	}

	TAILQ_INSERT_TAIL(&rtransport->devices, device, link);
	SPDK_DEBUGLOG(rdma, "New device %p is added to RDMA transport\n", device);

	if (g_nvmf_hooks.get_ibv_pd) {
		device->pd = g_nvmf_hooks.get_ibv_pd(NULL, device->context);
	} else {
		device->pd = spdk_rdma_utils_get_pd(device->context);
	}

	if (!device->pd) {
		SPDK_ERRLOG("Unable to allocate protection domain.\n");
		destroy_ib_device(rtransport, device);
		return -ENOMEM;
	}

	assert(device->map == NULL);

	device->map = spdk_rdma_utils_create_mem_map(device->pd, &g_nvmf_hooks, IBV_ACCESS_LOCAL_WRITE);
	if (!device->map) {
		SPDK_ERRLOG("Unable to allocate memory map for listen address\n");
		destroy_ib_device(rtransport, device);
		return -ENOMEM;
	}

	assert(device->map != NULL);
	assert(device->pd != NULL);

	pthread_mutex_init(&device->xrc_lock, NULL);
	RB_INIT(&device->xrc_tgts);
	TAILQ_INIT(&device->xrc_sessions);
	if (rtransport->rdma_opts.xrc) {
		struct ibv_xrcd_init_attr xattr = {
			.comp_mask = IBV_XRCD_INIT_ATTR_FD | IBV_XRCD_INIT_ATTR_OFLAGS,
			.fd = -1,
			.oflags = O_CREAT,
		};

		device->xrcd = ibv_open_xrcd(device->context, &xattr);
		if (device->xrcd == NULL) {
			SPDK_ERRLOG("XRC: ibv_open_xrcd on %s failed: %s\n",
				    ibv_get_device_name(context->device), spdk_strerror(errno));
			destroy_ib_device(rtransport, device);
			return -ENOMEM;
		}
	}

	if (new_device) {
		*new_device = device;
	}
	SPDK_NOTICELOG("Create IB device %s(%p/%p) succeed.\n", ibv_get_device_name(context->device),
		       device, context);

	return 0;
}

static void
free_poll_fds(struct spdk_nvmf_rdma_transport *rtransport)
{
	if (rtransport->poll_fds) {
		free(rtransport->poll_fds);
		rtransport->poll_fds = NULL;
	}
	rtransport->npoll_fds = 0;
}

static int
generate_poll_fds(struct spdk_nvmf_rdma_transport *rtransport)
{
	/* Set up poll descriptor array to monitor events from RDMA and IB
	 * in a single poll syscall
	 */
	int device_count = 0;
	int i = 0;
	struct spdk_nvmf_rdma_device *device, *tmp;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		device_count++;
	}

	rtransport->npoll_fds = device_count + 1;

	rtransport->poll_fds = calloc(rtransport->npoll_fds, sizeof(struct pollfd));
	if (rtransport->poll_fds == NULL) {
		SPDK_ERRLOG("poll_fds allocation failed\n");
		return -ENOMEM;
	}

	rtransport->poll_fds[i].fd = rtransport->event_channel->fd;
	rtransport->poll_fds[i++].events = POLLIN;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		rtransport->poll_fds[i].fd = device->context->async_fd;
		rtransport->poll_fds[i++].events = POLLIN;
	}

	return 0;
}

static struct spdk_nvmf_transport *
nvmf_rdma_create(struct spdk_nvmf_transport_opts *opts)
{
	int rc;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct ibv_context		**contexts;
	size_t				data_wr_pool_size;
	uint32_t			i;
	uint32_t			sge_count;
	uint32_t			min_shared_buffers;
	uint32_t			min_in_capsule_data_size;
	int				max_device_sge = SPDK_NVMF_MAX_SGL_ENTRIES;

	rtransport = calloc(1, sizeof(*rtransport));
	if (!rtransport) {
		return NULL;
	}

	TAILQ_INIT(&rtransport->devices);
	TAILQ_INIT(&rtransport->ports);
	TAILQ_INIT(&rtransport->poll_groups);
	TAILQ_INIT(&rtransport->retry_ports);

	rtransport->transport.ops = &spdk_nvmf_transport_rdma;
	rtransport->rdma_opts.num_cqe = DEFAULT_NVMF_RDMA_CQ_SIZE;
	rtransport->rdma_opts.max_srq_depth = SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH;
	rtransport->rdma_opts.no_srq = SPDK_NVMF_RDMA_DEFAULT_NO_SRQ;
	rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	rtransport->rdma_opts.no_wr_batching = SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING;
	rtransport->rdma_opts.in_capsule_data_disabled = false;
	rtransport->rdma_opts.xrc = false;
	rtransport->rdma_opts.xrc_srq_depth = 0;
	rtransport->rdma_opts.xrc_ini_send_depth = 4096;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, rdma_transport_opts_decoder,
					    SPDK_COUNTOF(rdma_transport_opts_decoder),
					    &rtransport->rdma_opts)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	SPDK_INFOLOG(rdma, "*** RDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, num_cqe=%d, max_srq_depth=%d, no_srq=%d,"
		     "  acceptor_backlog=%d, no_wr_batching=%d abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     rtransport->rdma_opts.num_cqe,
		     rtransport->rdma_opts.max_srq_depth,
		     rtransport->rdma_opts.no_srq,
		     rtransport->rdma_opts.acceptor_backlog,
		     rtransport->rdma_opts.no_wr_batching,
		     opts->abort_timeout_sec);

	if (rtransport->rdma_opts.xrc) {
		if (rtransport->rdma_opts.no_srq) {
			SPDK_ERRLOG("XRC needs shared receive queues (no_srq must be false)\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
		if (rtransport->rdma_opts.xrc_srq_depth == 0) {
			rtransport->rdma_opts.xrc_srq_depth = rtransport->rdma_opts.max_srq_depth;
		}
		SPDK_NOTICELOG("XRC enabled: command SRQ depth %u per poll group, INI QP send depth %u\n",
			       rtransport->rdma_opts.xrc_srq_depth, rtransport->rdma_opts.xrc_ini_send_depth);
	}

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	if (rtransport->rdma_opts.acceptor_backlog <= 0) {
		SPDK_ERRLOG("The acceptor backlog cannot be less than 1, setting to the default value of (%d).\n",
			    SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG);
		rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	}
	if (rtransport->transport.opts.msdbd > NVMF_DEFAULT_MSDBD) {
		SPDK_WARNLOG("Configured MSDBD %u exceeds max supported value, result is limited by %u\n",
			     rtransport->transport.opts.msdbd, NVMF_DEFAULT_MSDBD);
		rtransport->transport.opts.msdbd = NVMF_DEFAULT_MSDBD;
	} else if (!rtransport->transport.opts.msdbd) {
		rtransport->transport.opts.msdbd = NVMF_DEFAULT_MSDBD;
	}

	if (opts->num_shared_buffers < (SPDK_NVMF_MAX_SGL_ENTRIES * 2)) {
		SPDK_ERRLOG("The number of shared data buffers (%d) is less than"
			    "the minimum number required to guarantee that forward progress can be made (%d)\n",
			    opts->num_shared_buffers, (SPDK_NVMF_MAX_SGL_ENTRIES * 2));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	/* If buf_cache_size == UINT32_MAX, we will dynamically pick a cache size later that we know will fit. */
	if (opts->buf_cache_size < UINT32_MAX) {
		min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
		if (min_shared_buffers > opts->num_shared_buffers) {
			SPDK_ERRLOG("There are not enough buffers to satisfy"
				    "per-poll group caches for each thread. (%" PRIu32 ")"
				    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
			SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
			nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
			return NULL;
		}
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > NVMF_DEFAULT_TX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	min_in_capsule_data_size = sizeof(struct spdk_nvme_sgl_descriptor) * SPDK_NVMF_MAX_SGL_ENTRIES;
	if (opts->in_capsule_data_size < min_in_capsule_data_size) {
		SPDK_WARNLOG("In capsule data size is set to %u, this is minimum size required to support msdbd=16\n",
			     min_in_capsule_data_size);
		opts->in_capsule_data_size = min_in_capsule_data_size;
		rtransport->rdma_opts.in_capsule_data_disabled = true;
	}

	rtransport->event_channel = rdma_create_event_channel();
	if (rtransport->event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", spdk_strerror(errno));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	if (spdk_fd_set_nonblock(rtransport->event_channel->fd) < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	data_wr_pool_size = opts->data_wr_pool_size;
	if (data_wr_pool_size < SPDK_NVMF_MAX_SGL_ENTRIES * 2 * spdk_env_get_core_count()) {
		data_wr_pool_size = SPDK_NVMF_MAX_SGL_ENTRIES * 2 * spdk_env_get_core_count();
		SPDK_NOTICELOG("data_wr_pool_size is changed to %zu to guarantee enough cache for handling "
			       "at least one IO in each core\n", data_wr_pool_size);
	}
	rtransport->data_wr_pool = spdk_mempool_create("spdk_nvmf_rdma_wr_data", data_wr_pool_size,
				   sizeof(struct spdk_nvmf_rdma_request_data), SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_NUMA_ID_ANY);
	if (!rtransport->data_wr_pool) {
		if (spdk_mempool_lookup("spdk_nvmf_rdma_wr_data") != NULL) {
			SPDK_ERRLOG("Unable to allocate work request pool for poll group: already exists\n");
			SPDK_ERRLOG("Probably running in multiprocess environment, which is "
				    "unsupported by the nvmf library\n");
		} else {
			SPDK_ERRLOG("Unable to allocate work request pool for poll group\n");
		}
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	i = 0;
	rc = 0;
	while (contexts[i] != NULL) {
		rc = create_ib_device(rtransport, contexts[i], &device);
		if (rc < 0) {
			break;
		}
		i++;
		max_device_sge = spdk_min(max_device_sge, device->attr.max_sge);
		device->is_ready = true;
	}
	rdma_free_devices(contexts);

	if (opts->io_unit_size * max_device_sge < opts->max_io_size) {
		/* divide and round up. */
		opts->io_unit_size = (opts->max_io_size + max_device_sge - 1) / max_device_sge;

		/* round up to the nearest 4k. */
		opts->io_unit_size = (opts->io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT - 1) & ~NVMF_DATA_BUFFER_MASK;

		opts->io_unit_size = spdk_max(opts->io_unit_size, SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE);
		SPDK_NOTICELOG("Adjusting the io unit size to fit the device's maximum I/O size. New I/O unit size %u\n",
			       opts->io_unit_size);
	}

	if (rc < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rc = generate_poll_fds(rtransport);
	if (rc < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rtransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_rdma_accept, &rtransport->transport,
				    opts->acceptor_poll_rate);
	if (!rtransport->accept_poller) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	return &rtransport->transport;
}

static void
destroy_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
		  struct spdk_nvmf_rdma_device *device)
{
	TAILQ_REMOVE(&rtransport->devices, device, link);
	if (device->xrcd != NULL) {
		if (!TAILQ_EMPTY(&device->xrc_sessions) || !RB_EMPTY(&device->xrc_tgts)) {
			SPDK_ERRLOG("XRC: device %p destroyed with live sessions or queues\n", device);
		}
		ibv_close_xrcd(device->xrcd);
		device->xrcd = NULL;
	}
	spdk_rdma_utils_free_mem_map(&device->map);
	if (device->pd) {
		if (!g_nvmf_hooks.get_ibv_pd) {
			spdk_rdma_utils_put_pd(device->pd);
		}
	}
	SPDK_DEBUGLOG(rdma, "IB device [%p] is destroyed.\n", device);
	free(device);
}

static void
nvmf_rdma_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	assert(w != NULL);

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	spdk_json_write_named_uint32(w, "max_srq_depth", rtransport->rdma_opts.max_srq_depth);
	spdk_json_write_named_bool(w, "no_srq", rtransport->rdma_opts.no_srq);
	if (rtransport->rdma_opts.no_srq == true) {
		spdk_json_write_named_int32(w, "num_cqe", rtransport->rdma_opts.num_cqe);
	}
	spdk_json_write_named_int32(w, "acceptor_backlog", rtransport->rdma_opts.acceptor_backlog);
	spdk_json_write_named_bool(w, "no_wr_batching", rtransport->rdma_opts.no_wr_batching);
	if (rtransport->rdma_opts.xrc) {
		spdk_json_write_named_bool(w, "xrc", true);
		spdk_json_write_named_uint32(w, "xrc_srq_depth", rtransport->rdma_opts.xrc_srq_depth);
		spdk_json_write_named_uint32(w, "xrc_ini_send_depth", rtransport->rdma_opts.xrc_ini_send_depth);
	}
	if (transport->opts.msdbd != NVMF_DEFAULT_MSDBD) {
		spdk_json_write_named_bool(w, "msdbd", transport->opts.msdbd);
	}
}

static int
nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
		  spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->retry_ports, port, link);
		free(port);
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->ports, port, link);
		rdma_destroy_id(port->id);
		free(port);
	}

	free_poll_fds(rtransport);

	if (rtransport->event_channel != NULL) {
		rdma_destroy_event_channel(rtransport->event_channel);
	}

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		destroy_ib_device(rtransport, device);
	}

	if (rtransport->data_wr_pool != NULL) {
		if (spdk_mempool_count(rtransport->data_wr_pool) != transport->opts.data_wr_pool_size) {
			SPDK_ERRLOG("transport wr pool count is %zu but should be %u\n",
				    spdk_mempool_count(rtransport->data_wr_pool),
				    transport->opts.max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES);
		}
	}

	spdk_mempool_free(rtransport->data_wr_pool);

	spdk_poller_unregister(&rtransport->accept_poller);
	free(rtransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
	return 0;
}

static int nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
				     struct spdk_nvme_transport_id *trid,
				     bool peer);

static bool nvmf_rdma_rescan_devices(struct spdk_nvmf_rdma_transport *rtransport);

static int
nvmf_rdma_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port	*port, *tmp_port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;
	long int			port_val;
	bool				is_retry = false;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -EINVAL;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	assert(rtransport->event_channel != NULL);

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		return -ENOMEM;
	}

	port->trid = trid;

	switch (trid->adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", trid->adrfam);
		free(port);
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	/* Range check the trsvcid. Fail in 3 cases:
	 * < 0: means that spdk_strtol hit an error
	 * 0: this results in ephemeral port which we don't want
	 * > 65535: port too high
	 */
	port_val = spdk_strtol(trid->trsvcid, 10);
	if (port_val <= 0 || port_val > 65535) {
		SPDK_ERRLOG("invalid trsvcid %s\n", trid->trsvcid);
		free(port);
		return -EINVAL;
	}

	rc = getaddrinfo(trid->traddr, trid->trsvcid, &hints, &res);
	if (rc) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(rc), rc);
		free(port);
		return -(abs(rc));
	}

	rc = rdma_create_id(rtransport->event_channel, &port->id, port, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		freeaddrinfo(res);
		free(port);
		return rc;
	}

	rc = rdma_bind_addr(port->id, res->ai_addr);
	freeaddrinfo(res);

	if (rc < 0) {
		TAILQ_FOREACH(tmp_port, &rtransport->retry_ports, link) {
			if (spdk_nvme_transport_id_compare(tmp_port->trid, trid) == 0) {
				is_retry = true;
				break;
			}
		}
		if (!is_retry) {
			SPDK_ERRLOG("rdma_bind_addr() failed\n");
		}
		rdma_destroy_id(port->id);
		free(port);
		return rc;
	}

	if (!port->id->verbs) {
		SPDK_ERRLOG("ibv_context is null\n");
		rdma_destroy_id(port->id);
		free(port);
		return -1;
	}

	rc = rdma_listen(port->id, rtransport->rdma_opts.acceptor_backlog);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_listen() failed\n");
		rdma_destroy_id(port->id);
		free(port);
		return rc;
	}

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->context == port->id->verbs && device->is_ready && !device->need_destroy) {
			port->device = device;
			break;
		}
	}
	if (!port->device) {
		SPDK_ERRLOG("Accepted a connection with verbs %p, but unable to find a corresponding device.\n",
			    port->id->verbs);
		rdma_destroy_id(port->id);
		free(port);
		nvmf_rdma_rescan_devices(rtransport);
		return -EINVAL;
	}

	SPDK_NOTICELOG("*** NVMe/RDMA Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	return 0;
}

static void
nvmf_rdma_stop_listen_ex(struct spdk_nvmf_transport *transport,
			 const struct spdk_nvme_transport_id *trid, bool need_retry)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (!need_retry) {
		TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, tmp) {
			if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
				TAILQ_REMOVE(&rtransport->retry_ports, port, link);
				free(port);
			}
		}
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
			SPDK_DEBUGLOG(rdma, "Port %s:%s removed. need retry: %d\n",
				      port->trid->traddr, port->trid->trsvcid, need_retry);
			TAILQ_REMOVE(&rtransport->ports, port, link);
			rdma_destroy_id(port->id);
			port->id = NULL;
			port->device = NULL;
			if (need_retry) {
				TAILQ_INSERT_TAIL(&rtransport->retry_ports, port, link);
			} else {
				free(port);
			}
			break;
		}
	}
}

static void
nvmf_rdma_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	nvmf_rdma_stop_listen_ex(transport, trid, false);
}

static void _nvmf_rdma_register_poller_in_group(void *c);
static void _nvmf_rdma_remove_poller_in_group(void *c);

static bool
nvmf_rdma_all_pollers_management_done(void *c)
{
	struct poller_manage_ctx	*ctx = c;
	int				counter;

	counter = __atomic_sub_fetch(ctx->inflight_op_counter, 1, __ATOMIC_SEQ_CST);
	SPDK_DEBUGLOG(rdma, "nvmf_rdma_all_pollers_management_done called. counter: %d, poller: %p\n",
		      counter, ctx->rpoller);

	if (counter == 0) {
		free((void *)ctx->inflight_op_counter);
	}
	free(ctx);

	return counter == 0;
}

static int
nvmf_rdma_manage_poller(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_device *device, bool *has_inflight, bool is_add)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_poll_group		*poll_group;
	struct poller_manage_ctx		*ctx;
	bool					found;
	int					*inflight_counter;
	spdk_msg_fn				do_fn;

	*has_inflight = false;
	do_fn = is_add ? _nvmf_rdma_register_poller_in_group : _nvmf_rdma_remove_poller_in_group;
	inflight_counter = calloc(1, sizeof(int));
	if (!inflight_counter) {
		SPDK_ERRLOG("Failed to allocate inflight counter when removing pollers\n");
		return -ENOMEM;
	}

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		(*inflight_counter)++;
	}

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		found = false;
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			if (rpoller->device == device) {
				found = true;
				break;
			}
		}
		if (found == is_add) {
			__atomic_fetch_sub(inflight_counter, 1, __ATOMIC_SEQ_CST);
			continue;
		}

		ctx = calloc(1, sizeof(struct poller_manage_ctx));
		if (!ctx) {
			SPDK_ERRLOG("Failed to allocate poller_manage_ctx when removing pollers\n");
			if (!*has_inflight) {
				free(inflight_counter);
			}
			return -ENOMEM;
		}

		ctx->rtransport = rtransport;
		ctx->rgroup = rgroup;
		ctx->rpoller = rpoller;
		ctx->device = device;
		ctx->thread = spdk_get_thread();
		ctx->inflight_op_counter = inflight_counter;
		*has_inflight = true;

		poll_group = rgroup->group.group;
		if (poll_group->thread != spdk_get_thread()) {
			spdk_thread_send_msg(poll_group->thread, do_fn, ctx);
		} else {
			do_fn(ctx);
		}
	}

	if (!*has_inflight) {
		free(inflight_counter);
	}

	return 0;
}

static void nvmf_rdma_handle_device_removal(struct spdk_nvmf_rdma_transport *rtransport,
		struct spdk_nvmf_rdma_device *device);

static struct spdk_nvmf_rdma_device *
nvmf_rdma_find_ib_device(struct spdk_nvmf_rdma_transport *rtransport,
			 struct ibv_context *context)
{
	struct spdk_nvmf_rdma_device	*device, *tmp_device;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp_device) {
		if (device->need_destroy) {
			continue;
		}

		if (strcmp(device->context->device->dev_name, context->device->dev_name) == 0) {
			return device;
		}
	}

	return NULL;
}

static bool
nvmf_rdma_check_devices_context(struct spdk_nvmf_rdma_transport *rtransport,
				struct ibv_context *context)
{
	struct spdk_nvmf_rdma_device	*old_device, *new_device;
	int				rc = 0;
	bool				has_inflight;

	old_device = nvmf_rdma_find_ib_device(rtransport, context);

	if (old_device) {
		if (old_device->context != context && !old_device->need_destroy && old_device->is_ready) {
			/* context may not have time to be cleaned when rescan. exactly one context
			 * is valid for a device so this context must be invalid and just remove it. */
			SPDK_WARNLOG("Device %p has a invalid context %p\n", old_device, old_device->context);
			old_device->need_destroy = true;
			nvmf_rdma_handle_device_removal(rtransport, old_device);
		}
		return false;
	}

	rc = create_ib_device(rtransport, context, &new_device);
	/* TODO: update transport opts. */
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create ib device for context: %s(%p)\n",
			    ibv_get_device_name(context->device), context);
		return false;
	}

	rc = nvmf_rdma_manage_poller(rtransport, new_device, &has_inflight, true);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add poller for device context: %s(%p)\n",
			    ibv_get_device_name(context->device), context);
		return false;
	}

	if (has_inflight) {
		new_device->is_ready = true;
	}

	return true;
}

static bool
nvmf_rdma_rescan_devices(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_device	*device;
	struct ibv_device		**ibv_device_list = NULL;
	struct ibv_context		**contexts = NULL;
	int				i = 0;
	int				num_dev = 0;
	bool				new_create = false, has_new_device = false;
	struct ibv_context		*tmp_verbs = NULL;

	/* do not rescan when any device is destroying, or context may be freed when
	 * regenerating the poll fds.
	 */
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->need_destroy) {
			return false;
		}
	}

	ibv_device_list = ibv_get_device_list(&num_dev);

	/* There is a bug in librdmacm. If verbs init failed in rdma_get_devices, it'll be
	 * marked as dead verbs and never be init again. So we need to make sure the
	 * verbs is available before we call rdma_get_devices. */
	if (num_dev >= 0) {
		for (i = 0; i < num_dev; i++) {
			tmp_verbs = ibv_open_device(ibv_device_list[i]);
			if (!tmp_verbs) {
				SPDK_WARNLOG("Failed to init ibv device %p, err %d. Skip rescan.\n", ibv_device_list[i], errno);
				break;
			}
			if (nvmf_rdma_find_ib_device(rtransport, tmp_verbs) == NULL) {
				SPDK_DEBUGLOG(rdma, "Find new verbs init ibv device %p(%s).\n", ibv_device_list[i],
					      tmp_verbs->device->dev_name);
				has_new_device = true;
			}
			ibv_close_device(tmp_verbs);
		}
		ibv_free_device_list(ibv_device_list);
		if (!tmp_verbs || !has_new_device) {
			return false;
		}
	}

	contexts = rdma_get_devices(NULL);

	for (i = 0; contexts && contexts[i] != NULL; i++) {
		new_create |= nvmf_rdma_check_devices_context(rtransport, contexts[i]);
	}

	if (new_create) {
		free_poll_fds(rtransport);
		generate_poll_fds(rtransport);
	}

	if (contexts) {
		rdma_free_devices(contexts);
	}

	return new_create;
}

static bool
nvmf_rdma_retry_listen_port(struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_port	*port, *tmp_port;
	int				rc = 0;
	bool				new_create = false;

	if (TAILQ_EMPTY(&rtransport->retry_ports)) {
		return false;
	}

	new_create = nvmf_rdma_rescan_devices(rtransport);

	TAILQ_FOREACH_SAFE(port, &rtransport->retry_ports, link, tmp_port) {
		rc = nvmf_rdma_listen(&rtransport->transport, port->trid, NULL);

		TAILQ_REMOVE(&rtransport->retry_ports, port, link);
		if (rc) {
			if (new_create) {
				SPDK_ERRLOG("Found new IB device but port %s:%s is still failed(%d) to listen.\n",
					    port->trid->traddr, port->trid->trsvcid, rc);
			}
			TAILQ_INSERT_TAIL(&rtransport->retry_ports, port, link);
			break;
		} else {
			SPDK_NOTICELOG("Port %s:%s come back\n", port->trid->traddr, port->trid->trsvcid);
			free(port);
		}
	}

	return true;
}

static void
nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_qpair *rqpair, bool drain)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;
	struct spdk_nvmf_rdma_resources *resources;

	/* First process requests which are waiting for response to be sent */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->sq->pending_rdma_send_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* We process I/O in the data transfer pending queue at the highest priority. */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->sq->pending_rdma_read_queue, state_link, req_tmp) {
		if (rdma_req->state != RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING) {
			/* Requests in this queue might be in state RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
			 * they are transmitting data over network but we keep them in the list to guarantee
			 * fair processing. */
			continue;
		}
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then RDMA writes since reads have stronger restrictions than writes */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->sq->pending_rdma_write_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then we handle request waiting on memory buffers. */
	STAILQ_FOREACH_SAFE(req, &rqpair->poller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then we handle request waiting on accel tasks. */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->poller->group->pending_accel_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	resources = rqpair->resources;
	while (!STAILQ_EMPTY(&resources->free_queue) && !STAILQ_EMPTY(&resources->incoming_queue)) {
		rdma_req = STAILQ_FIRST(&resources->free_queue);
		STAILQ_REMOVE_HEAD(&resources->free_queue, state_link);
		rdma_req->recv = STAILQ_FIRST(&resources->incoming_queue);
		STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);

		if (rqpair->srq != NULL) {
			rdma_req->req.qpair = &rdma_req->recv->qpair->qpair;
			rdma_req->recv->qpair->qd++;
		} else {
			rqpair->qd++;
		}

		rdma_req->receive_tsc = rdma_req->recv->receive_tsc;
		rdma_req->state = RDMA_REQUEST_STATE_NEW;
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
	if (!STAILQ_EMPTY(&resources->incoming_queue) && STAILQ_EMPTY(&resources->free_queue)) {
		rqpair->poller->stat.pending_free_request++;
	}
}

static void
nvmf_rdma_poller_process_pending_buf_queue(struct spdk_nvmf_rdma_transport *rtransport,
		struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request *rdma_req;

	STAILQ_FOREACH_SAFE(req, &rpoller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
}

static inline bool
nvmf_rdma_device_supports_last_wqe_reached(struct spdk_nvmf_rdma_device *device)
{
	/* iWARP transport and SoftRoCE driver don't support LAST_WQE_REACHED ibv async event */
	return !nvmf_rdma_is_rxe_device(device) &&
	       device->context->device->transport_type != IBV_TRANSPORT_IWARP;
}

static void
nvmf_rdma_destroy_drained_qpair(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	if (nvmf_rdma_is_xrc_shadow(rqpair)) {
		if (!spdk_nvmf_qpair_is_active(&rqpair->qpair)) {
			nvmf_rdma_xrc_shadow_schedule_destroy(rqpair);
		}
		return;
	}

	if (rqpair->in_destroy) {
		return;
	}
	rqpair->in_destroy = true;

	if (rqpair->poller) {
		/* a qpair might be destroyed before being added to a poll group */
		nvmf_rdma_qpair_process_pending(rtransport, rqpair, true);
	}

	/* nvmf_rdma_close_qpair is not called */
	if (!rqpair->to_close) {
		goto again;
	}

	/* device is already destroyed and we should force destroy this qpair. */
	if (rqpair->poller && rqpair->poller->need_destroy) {
		nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	if (rqpair->xrc != NULL) {
		/* The INI QP is shared with the host's other queues: wait for this
		 * queue's own requests and for its shadows on other poll groups.
		 * Commands still in flight to the closed TGT QP are dropped on
		 * arrival (no queue for them any more). */
		if (rqpair->qd != 0 || nvmf_rdma_xrc_real_has_shadows(rqpair)) {
			goto again;
		}
		assert(rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED ||
		       rqpair->qpair.state == SPDK_NVMF_QPAIR_ERROR);
		nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	/* In non SRQ path, we will reach rqpair->max_queue_depth. In SRQ path, we will get the last_wqe event. */
	if (rqpair->sq->current_send_depth != 0) {
		goto again;
	}

	if (rqpair->srq == NULL && rqpair->current_recv_depth != rqpair->max_queue_depth) {
		goto again;
	}

	/* For devices that support LAST_WQE_REACHED with srq, we need to
	 * wait to destroy the qpair until that event has been received.
	 */
	if (rqpair->srq != NULL && rqpair->last_wqe_reached == false &&
	    nvmf_rdma_device_supports_last_wqe_reached(rqpair->device)) {
		goto again;
		return;
	}

	assert(rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED ||
	       rqpair->qpair.state == SPDK_NVMF_QPAIR_ERROR);

	nvmf_rdma_qpair_destroy(rqpair);

	return;
again:
	rqpair->in_destroy = false;
}

static int
nvmf_rdma_disconnect(struct rdma_cm_event *evt, bool *event_acked)
{
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;

	if (evt->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		return -1;
	}

	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}

	rdma_ack_cm_event(evt);
	*event_acked = true;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	spdk_trace_record(TRACE_RDMA_QP_DISCONNECT, 0, 0, (uintptr_t)rqpair);

	nvmf_rdma_qpair_disconnect(rqpair);

	return 0;
}

#ifdef DEBUG
static const char *CM_EVENT_STR[] = {
	"RDMA_CM_EVENT_ADDR_RESOLVED",
	"RDMA_CM_EVENT_ADDR_ERROR",
	"RDMA_CM_EVENT_ROUTE_RESOLVED",
	"RDMA_CM_EVENT_ROUTE_ERROR",
	"RDMA_CM_EVENT_CONNECT_REQUEST",
	"RDMA_CM_EVENT_CONNECT_RESPONSE",
	"RDMA_CM_EVENT_CONNECT_ERROR",
	"RDMA_CM_EVENT_UNREACHABLE",
	"RDMA_CM_EVENT_REJECTED",
	"RDMA_CM_EVENT_ESTABLISHED",
	"RDMA_CM_EVENT_DISCONNECTED",
	"RDMA_CM_EVENT_DEVICE_REMOVAL",
	"RDMA_CM_EVENT_MULTICAST_JOIN",
	"RDMA_CM_EVENT_MULTICAST_ERROR",
	"RDMA_CM_EVENT_ADDR_CHANGE",
	"RDMA_CM_EVENT_TIMEWAIT_EXIT"
};
#endif /* DEBUG */

static void
nvmf_rdma_disconnect_qpairs_on_port(struct spdk_nvmf_rdma_transport *rtransport,
				    struct spdk_nvmf_rdma_port *port)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_qpair		*rqpair;

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
				if (rqpair->listen_id == port->id) {
					nvmf_rdma_qpair_disconnect(rqpair);
				}
			}
		}
	}
}

static bool
nvmf_rdma_handle_cm_event_addr_change(struct spdk_nvmf_transport *transport,
				      struct rdma_cm_event *event)
{
	const struct spdk_nvme_transport_id	*trid;
	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_transport		*rtransport;
	bool					event_acked = false;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (port->id == event->id) {
			SPDK_ERRLOG("ADDR_CHANGE: IP %s:%s migrated\n", port->trid->traddr, port->trid->trsvcid);
			rdma_ack_cm_event(event);
			event_acked = true;
			trid = port->trid;
			break;
		}
	}

	if (event_acked) {
		nvmf_rdma_disconnect_qpairs_on_port(rtransport, port);

		nvmf_rdma_stop_listen(transport, trid);
		nvmf_rdma_listen(transport, trid, NULL);
	}

	return event_acked;
}

static void
nvmf_rdma_handle_device_removal(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	int				rc;
	bool				has_inflight;

	rc = nvmf_rdma_manage_poller(rtransport, device, &has_inflight, false);
	if (rc) {
		SPDK_ERRLOG("Failed to handle device removal, rc %d\n", rc);
		return;
	}

	if (!has_inflight) {
		/* no pollers, destroy the device */
		device->ready_to_destroy = true;
		spdk_thread_send_msg(spdk_get_thread(), _nvmf_rdma_remove_destroyed_device, rtransport);
	}

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		if (port->device == device) {
			SPDK_NOTICELOG("Port %s:%s on device %s is being removed.\n",
				       port->trid->traddr,
				       port->trid->trsvcid,
				       ibv_get_device_name(port->device->context->device));

			/* keep NVMF listener and only destroy structures of the
			 * RDMA transport. when the device comes back we can retry listening
			 * and the application's workflow will not be interrupted.
			 */
			nvmf_rdma_stop_listen_ex(&rtransport->transport, port->trid, true);
		}
	}
}

static void
nvmf_rdma_handle_cm_event_port_removal(struct spdk_nvmf_transport *transport,
				       struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_port		*port, *tmp_port;
	struct spdk_nvmf_rdma_transport		*rtransport;

	port = event->id->context;
	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rdma_ack_cm_event(event);

	/* if device removal happens during ctrl qpair disconnecting, it's possible that we receive
	 * an DEVICE_REMOVAL event on qpair but the id->qp is just NULL. So we should make sure that
	 * we are handling a port event here.
	 */
	TAILQ_FOREACH(tmp_port, &rtransport->ports, link) {
		if (port == tmp_port && port->device && !port->device->need_destroy) {
			port->device->need_destroy = true;
			nvmf_rdma_handle_device_removal(rtransport, port->device);
		}
	}
}

static void
nvmf_process_cm_events(struct spdk_nvmf_transport *transport, uint32_t max_events)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct rdma_cm_event		*event;
	uint32_t			i;
	int				rc;
	bool				event_acked;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (rtransport->event_channel == NULL) {
		return;
	}

	for (i = 0; i < max_events; i++) {
		event_acked = false;
		rc = rdma_get_cm_event(rtransport->event_channel, &event);
		if (rc) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}

		SPDK_DEBUGLOG(rdma, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

		spdk_trace_record(TRACE_RDMA_CM_ASYNC_EVENT, 0, 0, 0, event->event);

		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
		case RDMA_CM_EVENT_ROUTE_ERROR:
			/* No action required. The target never attempts to resolve routes. */
			break;
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			rc = nvmf_rdma_connect(transport, event);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process connect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			/* The target never initiates a new connection. So this will not occur. */
			break;
		case RDMA_CM_EVENT_CONNECT_ERROR:
			/* Can this happen? The docs say it can, but not sure what causes it. */
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			/* These only occur on the client side. */
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			/* TODO: Should we be waiting for this event anywhere? */
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			rc = nvmf_rdma_disconnect(event, &event_acked);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			/* In case of device removal, kernel IB part triggers IBV_EVENT_DEVICE_FATAL
			 * which triggers RDMA_CM_EVENT_DEVICE_REMOVAL on all cma_id’s.
			 * Once these events are sent to SPDK, we should release all IB resources and
			 * don't make attempts to call any ibv_query/modify/create functions. We can only call
			 * ibv_destroy* functions to release user space memory allocated by IB. All kernel
			 * resources are already cleaned. */
			if (event->id->qp) {
				/* If rdma_cm event has a valid `qp` pointer then the event refers to the
				 * corresponding qpair. Otherwise the event refers to a listening device. */
				rc = nvmf_rdma_disconnect(event, &event_acked);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
					break;
				}
			} else {
				nvmf_rdma_handle_cm_event_port_removal(transport, event);
				event_acked = true;
			}
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			/* Multicast is not used */
			break;
		case RDMA_CM_EVENT_ADDR_CHANGE:
			event_acked = nvmf_rdma_handle_cm_event_addr_change(transport, event);
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			/* For now, do nothing. The target never re-uses queue pairs. */
			break;
		default:
			SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
			break;
		}
		if (!event_acked) {
			rdma_ack_cm_event(event);
		}
	}
}

static void
nvmf_rdma_handle_last_wqe_reached(struct spdk_nvmf_rdma_qpair *rqpair)
{
	rqpair->last_wqe_reached = true;
	nvmf_rdma_destroy_drained_qpair(rqpair);
}

static void
nvmf_rdma_qpair_process_last_wqe_event(void *ctx)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *event_ctx = ctx;
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = event_ctx->rqpair;

	if (rqpair) {
		assert(event_ctx == rqpair->last_wqe_reached_ctx);
		rqpair->last_wqe_reached_ctx = NULL;
		nvmf_rdma_handle_last_wqe_reached(rqpair);
	}
	free(event_ctx);
}

static int
nvmf_rdma_send_qpair_last_wqe_event(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;
	struct spdk_thread *thr = NULL;
	int rc;

	if (rqpair->qpair.group) {
		thr = rqpair->qpair.group->thread;
	} else if (rqpair->destruct_channel) {
		thr = spdk_io_channel_get_thread(rqpair->destruct_channel);
	}

	if (!thr) {
		SPDK_DEBUGLOG(rdma, "rqpair %p has no thread\n", rqpair);
		return -EINVAL;
	}

	if (rqpair->last_wqe_reached || rqpair->last_wqe_reached_ctx != NULL) {
		SPDK_ERRLOG("LAST_WQE_REACHED already received for rqpair %p\n", rqpair);
		return -EALREADY;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->rqpair = rqpair;
	rqpair->last_wqe_reached_ctx = ctx;

	rc = spdk_thread_send_msg(thr, nvmf_rdma_qpair_process_last_wqe_event, ctx);
	if (rc) {
		rqpair->last_wqe_reached_ctx = NULL;
		free(ctx);
	}

	return rc;
}

/*
 * Async event on an XRC TGT or INI QP (their qp_context is NULL). A TGT QP
 * error ends its queue. An INI QP error also shows up as failed completions
 * on its poll group, which disconnect the queues using it; here it is logged.
 */
static void
nvmf_rdma_xrc_async_event(struct spdk_nvmf_rdma_device *device, struct ibv_async_event *event)
{
	struct spdk_nvmf_rdma_xrc_qpair find = {}, *xrc;
	struct spdk_nvmf_rdma_qpair *real = NULL;

	find.tgt_qpn = event->element.qp->qp_num;
	pthread_mutex_lock(&device->xrc_lock);
	xrc = RB_FIND(xrc_tgt_tree, &device->xrc_tgts, &find);
	if (xrc != NULL && !xrc->closing) {
		real = xrc->rqpair;
	}
	pthread_mutex_unlock(&device->xrc_lock);

	SPDK_NOTICELOG("XRC: async event %s on %s QP 0x%x\n", ibv_event_type_str(event->event_type),
		       xrc != NULL ? "TGT" : "INI or unknown", event->element.qp->qp_num);
	if (real != NULL && (event->event_type == IBV_EVENT_QP_FATAL ||
			     event->event_type == IBV_EVENT_QP_REQ_ERR ||
			     event->event_type == IBV_EVENT_QP_ACCESS_ERR)) {
		spdk_nvmf_qpair_disconnect(&real->qpair);
	}
}

static int
nvmf_process_ib_event(struct spdk_nvmf_rdma_device *device)
{
	int				rc;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct ibv_async_event		event;

	rc = ibv_get_async_event(device->context, &event);

	if (rc) {
		/* In non-blocking mode -1 means there are no events available */
		return rc;
	}

	switch (event.event_type) {
	case IBV_EVENT_QP_FATAL:
	case IBV_EVENT_QP_LAST_WQE_REACHED:
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_PATH_MIG_ERR:
		rqpair = event.element.qp->qp_context;
		if (!rqpair) {
			if (device->xrcd != NULL) {
				nvmf_rdma_xrc_async_event(device, &event);
				break;
			}
			/* Any QP event for NVMe-RDMA initiator may be returned. */
			SPDK_NOTICELOG("Async QP event for unknown QP: %s\n",
				       ibv_event_type_str(event.event_type));
			break;
		}

		switch (event.event_type) {
		case IBV_EVENT_QP_FATAL:
			SPDK_ERRLOG("Fatal event received for rqpair %p\n", rqpair);
			spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
					  (uintptr_t)rqpair, event.event_type);
			rqpair->ibv_in_error_state = true;
			nvmf_rdma_qpair_disconnect(rqpair);
			break;
		case IBV_EVENT_QP_LAST_WQE_REACHED:
			/* This event only occurs for shared receive queues. */
			SPDK_DEBUGLOG(rdma, "Last WQE reached event received for rqpair %p\n", rqpair);
			rc = nvmf_rdma_send_qpair_last_wqe_event(rqpair);
			if (rc) {
				SPDK_WARNLOG("Failed to send LAST_WQE_REACHED event. rqpair %p, err %d\n", rqpair, rc);
				rqpair->last_wqe_reached = true;
			}
			break;
		case IBV_EVENT_QP_REQ_ERR:
		case IBV_EVENT_QP_ACCESS_ERR:
		case IBV_EVENT_COMM_EST:
		case IBV_EVENT_PATH_MIG:
		case IBV_EVENT_PATH_MIG_ERR:
			SPDK_NOTICELOG("Async QP event: %s\n",
				       ibv_event_type_str(event.event_type));
			spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
					  (uintptr_t)rqpair, event.event_type);
			rqpair->ibv_in_error_state = true;
			break;
		default:
			break;
		}
		break;
	case IBV_EVENT_DEVICE_FATAL:
		SPDK_ERRLOG("Device Fatal event[%s] received on %s. device: %p\n",
			    ibv_event_type_str(event.event_type), ibv_get_device_name(device->context->device), device);
		device->need_destroy = true;
		break;
	case IBV_EVENT_CQ_ERR:
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_PORT_ERR:
	case IBV_EVENT_LID_CHANGE:
	case IBV_EVENT_PKEY_CHANGE:
	case IBV_EVENT_SM_CHANGE:
	case IBV_EVENT_SRQ_ERR:
	case IBV_EVENT_SRQ_LIMIT_REACHED:
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_GID_CHANGE:
	case IBV_EVENT_SQ_DRAINED:
	default:
		SPDK_NOTICELOG("Async event: %s\n",
			       ibv_event_type_str(event.event_type));
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0, 0, event.event_type);
		break;
	}
	ibv_ack_async_event(&event);

	return 0;
}

static void
nvmf_process_ib_events(struct spdk_nvmf_rdma_device *device, uint32_t max_events)
{
	int rc = 0;
	uint32_t i = 0;

	for (i = 0; i < max_events; i++) {
		rc = nvmf_process_ib_event(device);
		if (rc) {
			break;
		}
	}

	SPDK_DEBUGLOG(rdma, "Device %s: %u events processed\n", device->context->device->name, i);
}

static int
nvmf_rdma_accept(void *ctx)
{
	int	nfds, i = 0;
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device *device, *tmp;
	uint32_t count;
	short revents;
	bool do_retry;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	do_retry = nvmf_rdma_retry_listen_port(rtransport);

	count = nfds = poll(rtransport->poll_fds, rtransport->npoll_fds, 0);

	if (nfds <= 0) {
		return do_retry ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
	}

	/* The first poll descriptor is RDMA CM event */
	if (rtransport->poll_fds[i++].revents & POLLIN) {
		nvmf_process_cm_events(transport, NVMF_RDMA_MAX_EVENTS_PER_POLL);
		nfds--;
	}

	if (nfds == 0) {
		return SPDK_POLLER_BUSY;
	}

	/* Second and subsequent poll descriptors are IB async events */
	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		revents = rtransport->poll_fds[i++].revents;
		if (revents & POLLIN) {
			if (spdk_likely(!device->need_destroy)) {
				nvmf_process_ib_events(device, NVMF_RDMA_MAX_EVENTS_PER_POLL);
				if (spdk_unlikely(device->need_destroy)) {
					nvmf_rdma_handle_device_removal(rtransport, device);
				}
			}
			nfds--;
		} else if (revents & POLLNVAL || revents & POLLHUP) {
			SPDK_ERRLOG("Receive unknown revent %x on device %p\n", (int)revents, device);
			nfds--;
		}
	}
	/* check all flagged fd's have been served */
	assert(nfds == 0);

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_rdma_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		     struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->nvmf_specific.msdbd = transport->opts.msdbd;

	/* Disable in-capsule data transfer for RDMA controller when dif_insert_or_strip is enabled
	since in-capsule data only works with NVME drives that support SGL memory layout */
	if (transport->opts.dif_insert_or_strip) {
		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	}

	if (cdata->nvmf_specific.ioccsz > ((sizeof(struct spdk_nvme_cmd) + 0x1000) / 16)) {
		SPDK_WARNLOG("RDMA is configured to support up to 16 SGL entries while in capsule"
			     " data is greater than 4KiB.\n");
		SPDK_WARNLOG("When used in conjunction with the NVMe-oF initiator from the Linux "
			     "kernel between versions 5.4 and 5.12 data corruption may occur for "
			     "writes that are not a multiple of 4KiB in size.\n");
	}
}

static void
nvmf_rdma_discover(struct spdk_nvmf_transport *transport,
		   struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static int
nvmf_rdma_poller_create(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_poll_group *rgroup, struct spdk_nvmf_rdma_device *device,
			struct spdk_nvmf_rdma_poller **out_poller)
{
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_rdma_provider_srq_init_attr	srq_init_attr;
	struct spdk_nvmf_rdma_resource_opts	opts;
	int					num_cqe;
	struct spdk_rdma_provider_cq_init_attr	cq_init_attr;

	poller = calloc(1, sizeof(*poller));
	if (!poller) {
		SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
		return -1;
	}

	poller->device = device;
	poller->group = rgroup;
	*out_poller = poller;

	RB_INIT(&poller->qpairs);
	STAILQ_INIT(&poller->qpairs_pending_send);
	STAILQ_INIT(&poller->qpairs_pending_recv);

	TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
	SPDK_DEBUGLOG(rdma, "Create poller %p on device %p in poll group %p.\n", poller, device, rgroup);
	if (rtransport->rdma_opts.no_srq == false && device->num_srq < device->attr.max_srq) {
		if ((int)rtransport->rdma_opts.max_srq_depth > device->attr.max_srq_wr) {
			SPDK_WARNLOG("Requested SRQ depth %u, max supported by dev %s is %d\n",
				     rtransport->rdma_opts.max_srq_depth, device->context->device->name, device->attr.max_srq_wr);
		}
		poller->max_srq_depth = spdk_min((int)rtransport->rdma_opts.max_srq_depth, device->attr.max_srq_wr);

		device->num_srq++;
		memset(&srq_init_attr, 0, sizeof(srq_init_attr));
		srq_init_attr.pd = device->pd;
		srq_init_attr.stats = &poller->stat.qp_stats.recv;
		srq_init_attr.srq_init_attr.attr.max_wr = poller->max_srq_depth;
		srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
		poller->srq = spdk_rdma_provider_srq_create(&srq_init_attr);
		if (!poller->srq) {
			SPDK_ERRLOG("Unable to create shared receive queue, errno %d\n", errno);
			return -1;
		}

		opts.qp = poller->srq;
		opts.map = device->map;
		opts.qpair = NULL;
		opts.shared = true;
		opts.max_queue_depth = poller->max_srq_depth;
		opts.in_capsule_data_size = rtransport->transport.opts.in_capsule_data_size;

		poller->resources = nvmf_rdma_resources_create(&opts);
		if (!poller->resources) {
			SPDK_ERRLOG("Unable to allocate resources for shared receive queue.\n");
			return -1;
		}
	}

	/*
	 * When using an srq, we can limit the completion queue at startup.
	 * The following formula represents the calculation:
	 * num_cqe = num_recv + num_data_wr + num_send_wr.
	 * where num_recv=num_data_wr=and num_send_wr=poller->max_srq_depth
	 */
	if (poller->srq) {
		num_cqe = poller->max_srq_depth * 3;
	} else {
		num_cqe = rtransport->rdma_opts.num_cqe;
	}
	if (rtransport->rdma_opts.xrc && device->xrcd != NULL) {
		/* The XRC SRQ shares the CQ: its receives plus the READs and SENDs
		 * of the commands it holds, as for the regular SRQ. */
		poller->max_xrc_srq_depth = spdk_min(rtransport->rdma_opts.xrc_srq_depth,
						     (uint32_t)device->attr.max_srq_wr);
		num_cqe += poller->max_xrc_srq_depth * 3;
		num_cqe = spdk_min(num_cqe, device->attr.max_cqe);
	}

	cq_init_attr.cqe		= num_cqe;
	cq_init_attr.comp_vector	= 0;
	cq_init_attr.cq_context		= poller;
	cq_init_attr.comp_channel	= NULL;
	cq_init_attr.pd			= device->pd;

	poller->cq = spdk_rdma_provider_cq_create(&cq_init_attr);
	if (!poller->cq) {
		SPDK_ERRLOG("Unable to create completion queue\n");
		return -1;
	}
	poller->num_cqe = num_cqe;

	if (poller->max_xrc_srq_depth != 0) {
		struct ibv_srq_init_attr_ex xattr = {};

		poller->xrc_srq = calloc(1, sizeof(*poller->xrc_srq));
		if (poller->xrc_srq == NULL) {
			return -1;
		}
		poller->xrc_srq->stats = &poller->stat.qp_stats.recv;
		poller->xrc_srq->shared_stats = true;
		xattr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD |
				  IBV_SRQ_INIT_ATTR_CQ | IBV_SRQ_INIT_ATTR_PD;
		xattr.srq_type = IBV_SRQT_XRC;
		xattr.xrcd = device->xrcd;
		xattr.cq = poller->cq->cq;
		xattr.pd = device->pd;
		xattr.attr.max_wr = poller->max_xrc_srq_depth;
		xattr.attr.max_sge = spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
		poller->xrc_srq->srq = ibv_create_srq_ex(device->context, &xattr);
		if (poller->xrc_srq->srq == NULL) {
			SPDK_ERRLOG("XRC: ibv_create_srq_ex failed: %s\n", spdk_strerror(errno));
			free(poller->xrc_srq);
			poller->xrc_srq = NULL;
			return -1;
		}
		if (ibv_get_srq_num(poller->xrc_srq->srq, &poller->xrc_srqn)) {
			SPDK_ERRLOG("XRC: ibv_get_srq_num failed\n");
			return -1;
		}

		opts.qp = poller->xrc_srq;
		opts.map = device->map;
		opts.qpair = NULL;
		opts.shared = true;
		opts.max_queue_depth = poller->max_xrc_srq_depth;
		opts.in_capsule_data_size = rtransport->transport.opts.in_capsule_data_size;
		poller->xrc_resources = nvmf_rdma_resources_create(&opts);
		if (!poller->xrc_resources) {
			SPDK_ERRLOG("XRC: unable to allocate resources for the command SRQ.\n");
			return -1;
		}
		SPDK_NOTICELOG("XRC: poll group %u on %s: command SRQN 0x%x depth %u, CQ %d\n",
			       rgroup->xrc_index, ibv_get_device_name(device->context->device),
			       poller->xrc_srqn, poller->max_xrc_srq_depth, num_cqe);
	}
	return 0;
}

static void
_nvmf_rdma_register_poller_in_group(void *c)
{
	struct spdk_nvmf_rdma_poller	*poller = NULL;
	struct poller_manage_ctx	*ctx = c;
	struct spdk_nvmf_rdma_device	*device;
	int				rc;

	rc = nvmf_rdma_poller_create(ctx->rtransport, ctx->rgroup, ctx->device, &poller);
	if (rc < 0 && poller) {
		nvmf_rdma_poller_destroy(poller);
	}

	device = ctx->device;
	if (nvmf_rdma_all_pollers_management_done(ctx)) {
		device->is_ready = true;
	}
}

static void nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport,
			    struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;
	int					rc;

	if (spdk_interrupt_mode_is_enabled()) {
		SPDK_ERRLOG("RDMA transport does not support interrupt mode\n");
		return NULL;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);
	STAILQ_INIT(&rgroup->pending_accel_queue);

	if (rtransport->rdma_opts.xrc) {
		if (rtransport->xrc_num_pgs >= SPDK_NVMF_RDMA_XRC_MAX_WORKERS) {
			SPDK_ERRLOG("XRC supports at most %d poll groups\n", SPDK_NVMF_RDMA_XRC_MAX_WORKERS);
			free(rgroup);
			return NULL;
		}
		rgroup->xrc_index = rtransport->xrc_num_pgs;
	}

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		rc = nvmf_rdma_poller_create(rtransport, rgroup, device, &poller);
		if (rc < 0) {
			nvmf_rdma_poll_group_destroy(&rgroup->group);
			return NULL;
		}
	}

	rgroup->accel_ch = spdk_accel_get_io_channel();
	if (spdk_unlikely(!rgroup->accel_ch)) {
		SPDK_ERRLOG("Cannot create accel_channel for rgroup=%p\n", rgroup);
		nvmf_rdma_poll_group_destroy(&rgroup->group);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&rtransport->poll_groups, rgroup, link);
	if (rtransport->rdma_opts.xrc) {
		rtransport->xrc_pgs[rgroup->xrc_index] = rgroup;
		rtransport->xrc_num_pgs++;
	}
	if (rtransport->conn_sched.next_admin_pg == NULL) {
		rtransport->conn_sched.next_admin_pg = rgroup;
		rtransport->conn_sched.next_io_pg = rgroup;
	}

	return &rgroup->group;
}

static uint32_t
nvmf_poll_group_get_io_qpair_count(struct spdk_nvmf_poll_group *pg)
{
	uint32_t count;

	/* Just assume that unassociated qpairs will eventually be io
	 * qpairs.  This is close enough for the use cases for this
	 * function.
	 */
	pthread_mutex_lock(&pg->mutex);
	count = pg->stat.current_io_qpairs + pg->current_unassociated_qpairs;
	pthread_mutex_unlock(&pg->mutex);

	return count;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group **pg;
	struct spdk_nvmf_transport_poll_group *result;
	uint32_t count;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);

	if (TAILQ_EMPTY(&rtransport->poll_groups)) {
		return NULL;
	}

	if (qpair->qid == 0) {
		pg = &rtransport->conn_sched.next_admin_pg;
	} else {
		struct spdk_nvmf_rdma_poll_group *pg_min, *pg_start, *pg_current;
		uint32_t min_value;

		pg = &rtransport->conn_sched.next_io_pg;
		pg_min = *pg;
		pg_start = *pg;
		pg_current = *pg;
		min_value = nvmf_poll_group_get_io_qpair_count(pg_current->group.group);

		while (1) {
			count = nvmf_poll_group_get_io_qpair_count(pg_current->group.group);

			if (count < min_value) {
				min_value = count;
				pg_min = pg_current;
			}

			pg_current = TAILQ_NEXT(pg_current, link);
			if (pg_current == NULL) {
				pg_current = TAILQ_FIRST(&rtransport->poll_groups);
			}

			if (pg_current == pg_start || min_value == 0) {
				break;
			}
		}
		*pg = pg_min;
	}

	assert(*pg != NULL);

	result = &(*pg)->group;

	*pg = TAILQ_NEXT(*pg, link);
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&rtransport->poll_groups);
	}

	return result;
}

static void
nvmf_rdma_poller_destroy(struct spdk_nvmf_rdma_poller *poller)
{
	struct spdk_nvmf_rdma_qpair	*qpair, *tmp_qpair;

	TAILQ_REMOVE(&poller->group->pollers, poller, link);
	RB_FOREACH_SAFE(qpair, qpairs_tree, &poller->qpairs, tmp_qpair) {
		if (nvmf_rdma_is_xrc_shadow(qpair)) {
			nvmf_rdma_xrc_shadow_destroy(qpair);
		} else {
			nvmf_rdma_qpair_destroy(qpair);
		}
	}

	if (poller->xrc_srq) {
		if (poller->xrc_recvs_held != 0) {
			SPDK_ERRLOG("XRC: poller destroyed with %u command buffers not reposted\n",
				    poller->xrc_recvs_held);
		}
		if (poller->xrc_resources) {
			nvmf_rdma_resources_destroy(poller->xrc_resources);
		}
		if (poller->xrc_srq->srq) {
			spdk_rdma_provider_srq_destroy(poller->xrc_srq);
		} else {
			free(poller->xrc_srq);
		}
		poller->xrc_srq = NULL;
	}

	if (poller->srq) {
		if (poller->resources) {
			nvmf_rdma_resources_destroy(poller->resources);
		}
		spdk_rdma_provider_srq_destroy(poller->srq);
		SPDK_DEBUGLOG(rdma, "Destroyed RDMA shared queue %p\n", poller->srq);
	}

	if (poller->cq) {
		spdk_rdma_provider_cq_destroy(poller->cq);
	}

	if (poller->destroy_cb) {
		poller->destroy_cb(poller->destroy_cb_ctx);
		poller->destroy_cb = NULL;
	}

	free(poller);
}

static void
nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup, *next_rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;
	struct spdk_nvmf_rdma_transport		*rtransport;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	if (!rgroup) {
		return;
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		nvmf_rdma_poller_destroy(poller);
	}

	if (rgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_rdma_poll_group_create()
		 * calls this function directly in a failure path. */
		free(rgroup);
		return;
	}

	rtransport = SPDK_CONTAINEROF(rgroup->group.transport, struct spdk_nvmf_rdma_transport, transport);

	next_rgroup = TAILQ_NEXT(rgroup, link);
	TAILQ_REMOVE(&rtransport->poll_groups, rgroup, link);
	if (next_rgroup == NULL) {
		next_rgroup = TAILQ_FIRST(&rtransport->poll_groups);
	}
	if (rtransport->conn_sched.next_admin_pg == rgroup) {
		rtransport->conn_sched.next_admin_pg = next_rgroup;
	}
	if (rtransport->conn_sched.next_io_pg == rgroup) {
		rtransport->conn_sched.next_io_pg = next_rgroup;
	}
	if (rgroup->accel_ch) {
		spdk_put_io_channel(rgroup->accel_ch);
	}

	free(rgroup);
}

static void
nvmf_rdma_qpair_reject_connection(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->cm_id != NULL) {
		nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
	}
}

static int
nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;
	int					rc;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	device = rqpair->device;

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	if (poller->need_destroy) {
		SPDK_ERRLOG("Poller is destroying.\n");
		return -1;
	}

	rqpair->poller = poller;
	rqpair->srq = rqpair->poller->srq;

	rc = nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		rqpair->poller = NULL;
		rqpair->srq = NULL;
		return -1;
	}

	RB_INSERT(qpairs_tree, &poller->qpairs, rqpair);

	if (rqpair->xrc != NULL) {
		rc = nvmf_rdma_xrc_accept(rqpair);
	} else {
		rc = nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	}
	if (rc) {
		/* Try to reject, but we probably can't */
		nvmf_rdma_qpair_reject_connection(rqpair);
		return -1;
	}

	return 0;
}

static int
nvmf_rdma_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	assert(group->transport->tgt != NULL);

	rqpair->destruct_channel = spdk_get_io_channel(group->transport->tgt);

	if (!rqpair->destruct_channel) {
		SPDK_WARNLOG("failed to get io_channel, qpair %p\n", qpair);
		return 0;
	}

	/* Sanity check that we get io_channel on the correct thread */
	if (qpair->group) {
		assert(qpair->group->thread == spdk_io_channel_get_thread(rqpair->destruct_channel));
	}

	return 0;
}

static int
nvmf_rdma_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);

	/*
	 * AER requests are freed when a qpair is destroyed. The recv corresponding to that request
	 * needs to be returned to the shared receive queue or the poll group will eventually be
	 * starved of RECV structures.
	 */
	if (rqpair->srq && rdma_req->recv) {
		int rc;
		struct ibv_recv_wr *bad_recv_wr;

		spdk_rdma_provider_srq_queue_recv_wrs(rqpair->srq, &rdma_req->recv->wr);
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		}
		if (rqpair->xrc != NULL) {
			rqpair->poller->xrc_recvs_held--;
			rdma_req->recv = NULL;
		}
	}

	_nvmf_rdma_request_free(rdma_req, rtransport);
	return 0;
}

static int
nvmf_rdma_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req,
			struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_qpair     *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
			struct spdk_nvmf_rdma_qpair, qpair);

	assert(rdma_req->req.accel_sequence == NULL || rqpair->start_accel_sequence);
	rdma_req->req.accel_sequence = NULL;
	if (spdk_unlikely(rqpair->ibv_in_error_state)) {
		/* The connection is dead. Move the request directly to the completed state. */
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	} else {
		/* The connection is alive, so process the request as normal */
		rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	}

	nvmf_rdma_request_process(rtransport, rdma_req);

	return 0;
}

static void
nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	rqpair->to_close = true;

	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		nvmf_rdma_qpair_reject_connection(rqpair);
	}
	if (rqpair->rdma_qp) {
		spdk_rdma_provider_qp_disconnect(rqpair->rdma_qp);
	}
	if (rqpair->xrc != NULL) {
		if (rqpair->cm_id != NULL && rqpair->qpair.state != SPDK_NVMF_QPAIR_UNINITIALIZED) {
			/* No QP is attached to the CM id: just send the DREQ. */
			rdma_disconnect(rqpair->cm_id);
		}
		if (nvmf_rdma_xrc_real_close(rqpair, cb_fn, cb_arg)) {
			/* cb_fn runs once the shadows are gone, then the destroy. */
			return;
		}
	}

	nvmf_rdma_destroy_drained_qpair(rqpair);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static struct spdk_nvmf_rdma_qpair *
get_rdma_qpair_from_wc(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_qpair find;

	find.qp_num = wc->qp_num;

	return RB_FIND(qpairs_tree, &rpoller->qpairs, &find);
}

#ifdef DEBUG
static int
nvmf_rdma_req_is_completing(struct spdk_nvmf_rdma_request *rdma_req)
{
	return rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST ||
	       rdma_req->state == RDMA_REQUEST_STATE_COMPLETING;
}
#endif

static void
_poller_reset_failed_recvs(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_recv_wr *bad_recv_wr,
			   int rc)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;

	SPDK_ERRLOG("Failed to post a recv for the poller %p with errno %d\n", rpoller, -rc);
	while (bad_recv_wr != NULL) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_recv_wr->wr_id;
		rdma_recv = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);

		rdma_recv->qpair->current_recv_depth++;
		bad_recv_wr = bad_recv_wr->next;
		SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rdma_recv->qpair, -rc);
		nvmf_rdma_qpair_disconnect(rdma_recv->qpair);
	}
}

static void
_qp_reset_failed_recvs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rqpair, -rc);
	while (bad_recv_wr != NULL) {
		bad_recv_wr = bad_recv_wr->next;
		rqpair->current_recv_depth++;
	}
	nvmf_rdma_qpair_disconnect(rqpair);
}

static void
_poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_recv_wr		*bad_recv_wr;
	int				rc;

	if (rpoller->xrc_srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->xrc_srq, &bad_recv_wr);
		if (spdk_unlikely(rc)) {
			_poller_reset_failed_recvs(rpoller, bad_recv_wr, rc);
		}
	}
	if (rpoller->srq) {
		rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->srq, &bad_recv_wr);
		if (spdk_unlikely(rc)) {
			_poller_reset_failed_recvs(rpoller, bad_recv_wr, rc);
		}
	} else {
		while (!STAILQ_EMPTY(&rpoller->qpairs_pending_recv)) {
			rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_recv);
			rc = spdk_rdma_provider_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);
			if (spdk_unlikely(rc)) {
				_qp_reset_failed_recvs(rqpair, bad_recv_wr, rc);
			}
			STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_recv, recv_link);
		}
	}
}

static void
_qp_reset_failed_sends(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_sq *sq, struct ibv_send_wr *bad_wr, int rc)
{
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;
	struct spdk_nvmf_rdma_request	*prev_rdma_req = NULL, *cur_rdma_req = NULL;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;

	SPDK_ERRLOG("Failed to post a send for the send queue %p with errno %d\n", sq, -rc);
	for (; bad_wr != NULL; bad_wr = bad_wr->next) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_wr->wr_id;
		assert(sq->current_send_depth > 0);
		sq->current_send_depth--;
		switch (bad_rdma_wr->type) {
		case RDMA_WR_TYPE_DATA:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, data_wr);
			if (bad_wr->opcode == IBV_WR_RDMA_READ) {
				assert(sq->current_read_depth > 0);
				sq->current_read_depth--;
			}
			break;
		case RDMA_WR_TYPE_SEND:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, rsp_wr);
			break;
		default:
			SPDK_ERRLOG("Found a RECV in the list of pending SEND requests for send queue %p\n", sq);
			prev_rdma_req = cur_rdma_req;
			continue;
		}

		if (prev_rdma_req == cur_rdma_req) {
			/* this request was handled by an earlier wr. i.e. we were performing an nvme read. */
			/* We only have to check against prev_wr since each requests wrs are contiguous in this list. */
			continue;
		}

		rqpair = SPDK_CONTAINEROF(cur_rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
		switch (cur_rdma_req->state) {
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			cur_rdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			STAILQ_INSERT_TAIL(&sq->pending_rdma_send_queue, cur_rdma_req, state_link);
			cur_rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
		case RDMA_REQUEST_STATE_COMPLETING:
			cur_rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			break;
		default:
			SPDK_ERRLOG("Found a request in a bad state %d when draining pending SEND requests for send queue %p\n",
				    cur_rdma_req->state, sq);
			continue;
		}

		nvmf_rdma_request_process(rtransport, cur_rdma_req);
		prev_rdma_req = cur_rdma_req;

		if (spdk_nvmf_qpair_is_active(&rqpair->qpair)) {
			/* Disconnect the connection. */
			nvmf_rdma_qpair_disconnect(rqpair);
		}
	}
}

static void
_poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_sq	*sq;
	struct ibv_send_wr		*bad_wr = NULL;
	int				rc;

	while (!STAILQ_EMPTY(&rpoller->qpairs_pending_send)) {
		sq = STAILQ_FIRST(&rpoller->qpairs_pending_send);
		rc = spdk_rdma_provider_qp_flush_send_wrs(sq->rdma_qp, &bad_wr);

		/* bad wr always points to the first wr that failed. */
		if (spdk_unlikely(rc)) {
			_qp_reset_failed_sends(rtransport, sq, bad_wr, rc);
		}
		STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_send, send_link);
	}
}

static const char *
nvmf_rdma_wr_type_str(enum spdk_nvmf_rdma_wr_type wr_type)
{
	switch (wr_type) {
	case RDMA_WR_TYPE_RECV:
		return "RECV";
	case RDMA_WR_TYPE_SEND:
		return "SEND";
	case RDMA_WR_TYPE_DATA:
		return "DATA";
	default:
		SPDK_ERRLOG("Unknown WR type %d\n", wr_type);
		SPDK_UNREACHABLE();
	}
}

static inline void
nvmf_rdma_log_wc_status(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	enum spdk_nvmf_rdma_wr_type wr_type = ((struct spdk_nvmf_rdma_wr *)wc->wr_id)->type;

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		SPDK_DEBUGLOG(rdma,
			      "Error on CQ %p, (qp state %d, in_error %d) request 0x%lu, type %s, status: (%d): %s\n",
			      rqpair->poller->cq, rqpair->qpair.state, rqpair->ibv_in_error_state, wc->wr_id,
			      nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	} else {
		SPDK_ERRLOG("Error on CQ %p, (qp state %d, in_error %d) request 0x%lu, type %s, status: (%d): %s\n",
			    rqpair->poller->cq, rqpair->qpair.state, rqpair->ibv_in_error_state, wc->wr_id,
			    nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	}
}

static bool
_nvmf_rdma_handle_sigerr(struct spdk_nvmf_rdma_transport *rtransport,
			 struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	uint32_t mkey, i, max_req_count;
	struct spdk_nvmf_rdma_request *rdma_req;

	assert((wc->wr_id >> 32) == 0);
	mkey = wc->wr_id;

	max_req_count = nvmf_rdma_qpair_max_reqs(rqpair);

	for (i = 0; i < max_req_count; i++) {
		struct spdk_nvme_cmd *cmd;
		struct spdk_nvme_cpl *rsp;

		rdma_req = &rqpair->resources->reqs[i];

		if (rdma_req->state == RDMA_REQUEST_STATE_FREE) {
			continue;
		}

		if (rdma_req->data_transfer_mkey == NULL) {
			continue;
		}

		if (spdk_rdma_provider_memory_key_get_key(rdma_req->data_transfer_mkey) == mkey) {
			continue;
		}

		cmd = &rdma_req->req.cmd->nvme_cmd;
		rsp = &rdma_req->req.rsp->nvme_cpl;

		SPDK_ERRLOG("req %p (lba %" PRIx64", num_blocks %x) got DIF error %d\n",
			    rdma_req, from_le64(&cmd->cdw10),
			    (from_le32(&cmd->cdw12) ^ 0xFFFFu) + 1, (int)wc->vendor_err);

		rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
		rsp->status.sc = nvmf_rdma_dif_error_to_compl_status(wc->vendor_err);

		return true;
	}

	return false;
}

static void
nvmf_rdma_handle_sigerr(struct spdk_nvmf_rdma_transport *rtransport,
			struct spdk_nvmf_rdma_poller *rpoller, struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_qpair *rqpair;

	RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
		if (_nvmf_rdma_handle_sigerr(rtransport, rqpair, wc)) {
			return;
		}
	}

	SPDK_ERRLOG("No req was found to signature error (mkey %x)\n", (uint32_t)wc->wr_id);
}

static int
nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
		      struct spdk_nvmf_rdma_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_rdma_wr	*rdma_wr;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_qpair	*rqpair, *tmp_rqpair;
	int reaped, i;
	int count = 0;
	int rc;
	bool error = false, fail_data_transfer = false;
	uint64_t poll_tsc = spdk_get_ticks();

	if (spdk_unlikely(rpoller->need_destroy)) {
		/* If qpair is closed before poller destroy, nvmf_rdma_destroy_drained_qpair may not
		 * be called because we cannot poll anything from cq. So we call that here to force
		 * destroy the qpair after to_close turning true.
		 */
		RB_FOREACH_SAFE(rqpair, qpairs_tree, &rpoller->qpairs, tmp_rqpair) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
		return 0;
	}

	/* Poll for completing operations. */
	reaped = spdk_rdma_provider_cq_poll(rpoller->cq, 32, wc);
	if (spdk_unlikely(reaped < 0)) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	} else if (reaped == 0) {
		rpoller->stat.idle_polls++;
	}

	rpoller->stat.polls++;
	rpoller->stat.completions += reaped;

	for (i = 0; i < reaped; i++) {

		if (spdk_unlikely((int)wc[i].status == SPDK_RDMA_PROVIDER_WC_SIG_ERR)) {
			nvmf_rdma_handle_sigerr(rtransport, rpoller, &wc[i]);
			continue;
		}

		rdma_wr = (struct spdk_nvmf_rdma_wr *)wc[i].wr_id;
		if (spdk_unlikely(!rdma_wr)) {
			assert(wc[i].status != IBV_WC_SUCCESS);
			continue;
		}

		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_SEND:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, rsp_wr);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			if (spdk_likely(!wc[i].status)) {
				count++;
				assert(wc[i].opcode == IBV_WC_SEND);
				assert(nvmf_rdma_req_is_completing(rdma_req));
			}

			rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			/* RDMA_WRITE operation completed. +1 since it was chained with rsp WR */
			assert(rqpair->sq->current_send_depth >= (uint32_t)rdma_req->num_outstanding_data_wr + 1);
			rqpair->sq->current_send_depth -= rdma_req->num_outstanding_data_wr + 1;
			rdma_req->num_outstanding_data_wr = 0;
			SPDK_DEBUGLOG(rdma, "req %p, C2H completed, transfer_cpl_cb %p\n", rdma_req,
				      rdma_req->transfer_cpl_cb);
			assert(rdma_req->transfer_cpl_cb == NULL);

			nvmf_rdma_request_process(rtransport, rdma_req);
			break;
		case RDMA_WR_TYPE_RECV:
			/* rdma_recv->qpair will be invalid if using an SRQ.  In that case we have to get the qpair from the wc. */
			rdma_recv = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);
			if (rdma_recv->srq != NULL && rdma_recv->srq == rpoller->xrc_srq) {
				/* XRC: wc.qp_num is the TGT QP, i.e. the host queue. */
				rdma_recv->qpair = get_rdma_qpair_from_wc(rpoller, &wc[i]);
				if (rdma_recv->qpair == NULL && spdk_likely(!wc[i].status)) {
					rdma_recv->qpair = nvmf_rdma_xrc_shadow_get(rpoller, wc[i].qp_num);
				}
				if (spdk_unlikely(rdma_recv->qpair == NULL)) {
					struct ibv_recv_wr *bad_wr;

					rpoller->stat.xrc_orphan_recvs++;
					rdma_recv->wr.next = NULL;
					spdk_rdma_provider_srq_queue_recv_wrs(rpoller->xrc_srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->xrc_srq, &bad_wr);
					if (rc) {
						SPDK_ERRLOG("Failed to re-post recv WR to XRC SRQ, err %d\n", rc);
					}
					continue;
				}
				rpoller->xrc_recvs_held++;
				rpoller->stat.xrc_requests++;
				rdma_recv->qpair->xrc->cmds++;
			} else if (rpoller->srq != NULL) {
				rdma_recv->qpair = get_rdma_qpair_from_wc(rpoller, &wc[i]);
				/* It is possible that there are still some completions for destroyed QP
				 * associated with SRQ. We just ignore these late completions and re-post
				 * receive WRs back to SRQ.
				 */
				if (spdk_unlikely(NULL == rdma_recv->qpair)) {
					struct ibv_recv_wr *bad_wr;

					rdma_recv->wr.next = NULL;
					spdk_rdma_provider_srq_queue_recv_wrs(rpoller->srq, &rdma_recv->wr);
					rc = spdk_rdma_provider_srq_flush_recv_wrs(rpoller->srq, &bad_wr);
					if (rc) {
						SPDK_ERRLOG("Failed to re-post recv WR to SRQ, err %d\n", rc);
					}
					continue;
				}
			}
			rqpair = rdma_recv->qpair;

			assert(rqpair != NULL);
			if (spdk_likely(!wc[i].status)) {
				assert(wc[i].opcode == IBV_WC_RECV);
				if (rqpair->current_recv_depth >= rqpair->max_queue_depth) {
					nvmf_rdma_qpair_disconnect(rqpair);
					break;
				}
			}

			rdma_recv->wr.next = NULL;
			rqpair->current_recv_depth++;
			rdma_recv->receive_tsc = poll_tsc;
			rpoller->stat.requests++;
			STAILQ_INSERT_HEAD(&rqpair->resources->incoming_queue, rdma_recv, link);
			rqpair->qpair.queue_depth++;
			break;
		case RDMA_WR_TYPE_DATA:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, data_wr);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(rdma_req->num_outstanding_data_wr > 0);

			rqpair->sq->current_send_depth--;
			rdma_req->num_outstanding_data_wr--;
			if (spdk_likely(!wc[i].status)) {
				if (wc[i].opcode == IBV_WC_RDMA_READ) {
					rqpair->sq->current_read_depth--;
					/* wait for all outstanding reads associated with the same rdma_req to complete before proceeding. */
					if (rdma_req->num_outstanding_data_wr == 0) {
						if (rdma_req->num_remaining_data_wr) {
							/* Only part of RDMA_READ operations was submitted, process the rest */
							nvmf_rdma_request_reset_transfer_in(rdma_req, rtransport);
							rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
							nvmf_rdma_request_process(rtransport, rdma_req);
							break;
						}
						SPDK_DEBUGLOG(rdma, "req %p, transfer_cpl_cb %p\n", rdma_req, rdma_req->transfer_cpl_cb);
						if (rdma_req->transfer_cpl_cb) {
							/* We need to wait for bdev layer to call req_complete */
							rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
							nvmf_rdma_req_finish_data_transfer(rdma_req, 0);
						} else {
							rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
							nvmf_rdma_request_process(rtransport, rdma_req);
						}
					}
				} else {
					/* That is only possible as part of async data transfer */
					assert(wc[i].opcode == IBV_WC_RDMA_WRITE);
					assert(rdma_req->transfer_cpl_cb);
					if (rdma_req->num_outstanding_data_wr == 0) {
						/* We need to wait for bdev layer to call req_complete */
						SPDK_DEBUGLOG(rdma, "req %p, finish data transfer\n", rdma_req);
						/* Set a flag that data is already transferred, we only need to send the response once bdev completes the req */
						rdma_req->data_transferred = true;
						rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
						nvmf_rdma_req_finish_data_transfer(rdma_req, 0);
					}
				}
			} else {
				rqpair->ibv_in_error_state = true;
				/* If the data transfer fails still force the queue into the error state,
				 * if we were performing an RDMA_READ, we need to force the request into a
				 * completed state since it wasn't linked to a send. However, in the RDMA_WRITE
				 * case, we should wait for the SEND to complete. */
				if (rdma_req->data.wr.opcode == IBV_WR_RDMA_READ) {
					rqpair->sq->current_read_depth--;
					if (rdma_req->num_outstanding_data_wr == 0) {
						if (rdma_req->num_remaining_data_wr) {
							/* Partially sent request is still in the pending_rdma_read_queue,
							 * remove it now before completing */
							rdma_req->num_remaining_data_wr = 0;
							NVMF_RDMA_SQ_READ_REMOVE(rqpair->sq, rdma_req);
						}
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						if (rdma_req->transfer_cpl_cb) {
							/* Request will be completed by ctrlr */
							fail_data_transfer = true;
						} else {
							nvmf_rdma_request_process(rtransport, rdma_req);
						}
					}
				} else if (rdma_req->data.wr.opcode == IBV_WR_RDMA_WRITE) {
					/* We can receive completion for WRITE as part of accel sequence handling */
					if (rdma_req->num_outstanding_data_wr == 0 && rdma_req->transfer_cpl_cb) {
						fail_data_transfer = true;
					}
				}
			}
			break;
		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			continue;
		}

		/* Handle error conditions */
		if (spdk_unlikely(wc[i].status)) {
			rqpair->ibv_in_error_state = true;
			nvmf_rdma_log_wc_status(rqpair, &wc[i]);
			if (rqpair->xrc != NULL && rdma_wr->type != RDMA_WR_TYPE_RECV) {
				nvmf_rdma_xrc_ini_failed(rpoller, rqpair->sq);
			}

			error = true;

			if (spdk_nvmf_qpair_is_active(&rqpair->qpair)) {
				/* Disconnect the connection. */
				nvmf_rdma_qpair_disconnect(rqpair);
			}
			if (fail_data_transfer) {
				fail_data_transfer = false;
				/* Completion of the data transfer triggers IO completion in the controller.
				 * As result, if qpair is in the process of disconnect and that was the last outstanding IO,
				 * the qpair can be destroyed via qpair_fini callback. We must not reference qpair after
				 * completing data transfer in error path to avoid heap-use after free or double free */
				nvmf_rdma_req_finish_data_transfer(rdma_req, -EIO);
			} else {
				nvmf_rdma_destroy_drained_qpair(rqpair);
			}
			continue;
		}

		nvmf_rdma_qpair_process_pending(rtransport, rqpair, false);

		if (spdk_unlikely(!spdk_nvmf_qpair_is_active(&rqpair->qpair))) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
	}

	if (spdk_unlikely(error == true)) {
		return -1;
	}

	if (reaped == 0) {
		/* In some cases we may not receive any CQE but we still may have pending IO requests waiting for
		 * a resource (e.g. a WR from the data_wr_pool).
		 * We need to start processing of such requests if no CQE reaped */
		nvmf_rdma_poller_process_pending_buf_queue(rtransport, rpoller);
	}

	/* submit outstanding work requests. */
	_poller_submit_recvs(rtransport, rpoller);
	_poller_submit_sends(rtransport, rpoller);

	return count;
}

static void
_nvmf_rdma_remove_destroyed_device(void *c)
{
	struct spdk_nvmf_rdma_transport	*rtransport = c;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;
	int				rc;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		if (device->ready_to_destroy) {
			destroy_ib_device(rtransport, device);
		}
	}

	free_poll_fds(rtransport);
	rc = generate_poll_fds(rtransport);
	/* cannot handle fd allocation error here */
	if (rc != 0) {
		SPDK_ERRLOG("Failed to generate poll fds after remove ib device.\n");
	}
}

static void
_nvmf_rdma_remove_poller_in_group_cb(void *c)
{
	struct poller_manage_ctx	*ctx = c;
	struct spdk_nvmf_rdma_transport	*rtransport = ctx->rtransport;
	struct spdk_nvmf_rdma_device	*device = ctx->device;
	struct spdk_thread		*thread = ctx->thread;

	if (nvmf_rdma_all_pollers_management_done(c)) {
		/* destroy device when last poller is destroyed */
		device->ready_to_destroy = true;
		spdk_thread_send_msg(thread, _nvmf_rdma_remove_destroyed_device, rtransport);
	}
}

static void
_nvmf_rdma_remove_poller_in_group(void *c)
{
	struct poller_manage_ctx		*ctx = c;
	struct spdk_nvmf_rdma_poller            *rpoller;

	/* Check if the poller referred to by this context was already removed.
	 * If it was already removed, simply call the usual destroy_cb function
	 * directly.
	 */
	TAILQ_FOREACH(rpoller, &ctx->rgroup->pollers, link) {
		if (rpoller == ctx->rpoller) {
			break;
		}
	}

	if (rpoller != NULL) {
		rpoller->need_destroy = true;
		rpoller->destroy_cb_ctx = ctx;
		rpoller->destroy_cb = _nvmf_rdma_remove_poller_in_group_cb;
		/* qp will be disconnected after receiving a RDMA_CM_EVENT_DEVICE_REMOVAL event. */
		if (RB_EMPTY(&ctx->rpoller->qpairs)) {
			nvmf_rdma_poller_destroy(ctx->rpoller);
		}
	} else {
		_nvmf_rdma_remove_poller_in_group_cb(ctx);
	}
}

static int
nvmf_rdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller	*rpoller, *tmp;
	int				count = 0, rc, rc2 = 0;

	rtransport = SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	TAILQ_FOREACH_SAFE(rpoller, &rgroup->pollers, link, tmp) {
		rc = nvmf_rdma_poller_poll(rtransport, rpoller);
		if (spdk_unlikely(rc < 0)) {
			if (rc2 == 0) {
				rc2 = rc;
			}
			continue;
		}
		count += rc;
	}

	return rc2 ? rc2 : count;
}

static int
nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
			  struct spdk_nvme_transport_id *trid,
			  bool peer)
{
	struct sockaddr *saddr;
	uint16_t port;

	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_RDMA);

	if (peer) {
		saddr = rdma_get_peer_addr(id);
	} else {
		saddr = rdma_get_local_addr(id);
	}
	switch (saddr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *saddr_in = (struct sockaddr_in *)saddr;

		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
		inet_ntop(AF_INET, &saddr_in->sin_addr,
			  trid->traddr, sizeof(trid->traddr));
		if (peer) {
			port = ntohs(rdma_get_dst_port(id));
		} else {
			port = ntohs(rdma_get_src_port(id));
		}
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *saddr_in = (struct sockaddr_in6 *)saddr;
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
		inet_ntop(AF_INET6, &saddr_in->sin6_addr,
			  trid->traddr, sizeof(trid->traddr));
		if (peer) {
			port = ntohs(rdma_get_dst_port(id));
		} else {
			port = ntohs(rdma_get_src_port(id));
		}
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	default:
		return -1;

	}

	return 0;
}

static int
nvmf_rdma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, true);
}

static int
nvmf_rdma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, false);
}

static int
nvmf_rdma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return nvmf_rdma_trid_from_cm_id(rqpair->listen_id, trid, false);
}

void
spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvmf_hooks = *hooks;
}

static void
nvmf_rdma_request_set_abort_status(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_rdma_request *rdma_req_to_abort,
				   struct spdk_nvmf_rdma_qpair *rqpair)
{
	rdma_req_to_abort->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	rdma_req_to_abort->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	/* Ensure cid is correct in case abort was requested before IO is being executed */
	rdma_req_to_abort->req.rsp->nvme_cpl.sqid = 0;
	rdma_req_to_abort->req.rsp->nvme_cpl.status.p = 0;
	rdma_req_to_abort->req.rsp->nvme_cpl.cid = rdma_req_to_abort->req.cmd->nvme_cmd.cid;

	STAILQ_INSERT_TAIL(&rqpair->sq->pending_rdma_send_queue, rdma_req_to_abort, state_link);
	rdma_req_to_abort->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING;

	req->rsp->nvme_cpl.cdw0 &= ~1U;	/* Command was successfully aborted. */
}

static int
_nvmf_rdma_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = SPDK_CONTAINEROF(
				req->req_to_abort, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (rdma_req_to_abort->state) {
	case RDMA_REQUEST_STATE_EXECUTING:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case RDMA_REQUEST_STATE_NEED_BUFFER:
		STAILQ_REMOVE(&rqpair->poller->group->group.pending_buf_queue,
			      &rdma_req_to_abort->req, spdk_nvmf_request, buf_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_NEED_ACCEL_TASK:
		STAILQ_REMOVE(&rqpair->poller->group->pending_accel_queue,
			      rdma_req_to_abort, spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
		NVMF_RDMA_SQ_READ_REMOVE(rqpair->sq, rdma_req_to_abort);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
		STAILQ_REMOVE(&rqpair->sq->pending_rdma_write_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING:
		/* Remove req from the list here to re-use common function */
		STAILQ_REMOVE(&rqpair->sq->pending_rdma_send_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort, rqpair);
		break;

	case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_rdma_qpair_abort_request, req, 0);
			return SPDK_POLLER_BUSY;
		}
		break;

	default:
		break;
	}

	spdk_nvmf_request_complete(req);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_rdma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_qpair *rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i, max_req_count;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = NULL, *rdma_req;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	transport = &rtransport->transport;

	if (rqpair->xrc != NULL) {
		/* The command may be running on any poll group. */
		nvmf_rdma_xrc_abort_request(rqpair, req);
		return;
	}

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	max_req_count = nvmf_rdma_qpair_max_reqs(rqpair);

	for (i = 0; i < max_req_count; i++) {
		rdma_req = &rqpair->resources->reqs[i];
		/* When SRQ == NULL, rqpair has its own requests and req.qpair pointer always points to the qpair
		 * When SRQ != NULL all rqpairs share common requests and qpair pointer is assigned when we start to
		 * process a request. So in both cases all requests which are not in FREE state have valid qpair ptr */
		if (rdma_req->state != RDMA_REQUEST_STATE_FREE && rdma_req->req.cmd->nvme_cmd.cid == cid &&
		    rdma_req->req.qpair == qpair) {
			rdma_req_to_abort = rdma_req;
			break;
		}
	}

	if (rdma_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &rdma_req_to_abort->req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_rdma_qpair_abort_request(req);
}

/* XRC state of one (poll group, device), for nvmf_get_stats. Everything read
 * here belongs to this poll group's thread, which runs the dump. */
static void
nvmf_rdma_xrc_dump_poller(struct spdk_nvmf_rdma_poller *rpoller, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_device *device = rpoller->device;
	struct spdk_nvmf_rdma_xrc_session *session;
	struct spdk_nvmf_rdma_qpair *rqpair;
	struct spdk_nvmf_rdma_xrc_ini *ini;
	struct spdk_nvmf_request *req;
	uint32_t worker = nvmf_rdma_poller_worker(rpoller), n;

	spdk_json_write_named_object_begin(w, "xrc");
	spdk_json_write_named_uint32(w, "worker", worker);
	spdk_json_write_named_uint32(w, "srqn", rpoller->xrc_srqn);
	spdk_json_write_named_uint32(w, "srq_depth", rpoller->max_xrc_srq_depth);
	spdk_json_write_named_uint32(w, "recvs_held", rpoller->xrc_recvs_held);
	spdk_json_write_named_uint64(w, "requests", rpoller->stat.xrc_requests);
	spdk_json_write_named_uint64(w, "orphan_recvs", rpoller->stat.xrc_orphan_recvs);
	spdk_json_write_named_uint64(w, "shadows_created", rpoller->stat.xrc_shadows_created);
	spdk_json_write_named_uint64(w, "shadows_destroyed", rpoller->stat.xrc_shadows_destroyed);

	spdk_json_write_named_array_begin(w, "ini_qps");
	pthread_mutex_lock(&device->xrc_lock);
	TAILQ_FOREACH(session, &device->xrc_sessions, link) {
		if (worker >= session->num_ini) {
			continue;
		}
		ini = &session->ini[worker];
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string_fmt(w, "session", "0x%" PRIx64, session->id);
		spdk_json_write_named_string(w, "hostnqn", session->hostnqn);
		spdk_json_write_named_uint32(w, "qpn", ini->qp ? ini->qp->qp_num : 0);
		spdk_json_write_named_uint32(w, "host_tgt_qpn", ini->host_tgt_qpn);
		spdk_json_write_named_bool(w, "in_error", ini->in_error);
		spdk_json_write_named_uint32(w, "send_depth", ini->sq.current_send_depth);
		spdk_json_write_named_uint32(w, "max_send_depth", ini->sq.max_send_depth);
		spdk_json_write_named_uint32(w, "read_depth", ini->sq.current_read_depth);
		spdk_json_write_named_uint32(w, "max_read_depth", ini->sq.max_read_depth);
		spdk_json_write_named_uint32(w, "pending_read", ini->sq.pending_read_len);
		spdk_json_write_named_uint32(w, "pending_read_peak", ini->sq.pending_read_peak);
		spdk_json_write_named_uint64(w, "read_stalls", ini->sq.read_stalls);
		spdk_json_write_named_bool(w, "pending_write_empty", STAILQ_EMPTY(&ini->sq.pending_rdma_write_queue));
		spdk_json_write_named_bool(w, "pending_send_empty", STAILQ_EMPTY(&ini->sq.pending_rdma_send_queue));
		spdk_json_write_object_end(w);
	}
	pthread_mutex_unlock(&device->xrc_lock);
	spdk_json_write_array_end(w);

	spdk_json_write_named_array_begin(w, "queues");
	RB_FOREACH(rqpair, qpairs_tree, &rpoller->qpairs) {
		if (rqpair->xrc == NULL) {
			continue;
		}
		n = 0;
		TAILQ_FOREACH(req, &rqpair->qpair.outstanding, link) {
			n++;
		}
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "qid", rqpair->qpair.qid);
		spdk_json_write_named_uint32(w, "cntlid", rqpair->qpair.ctrlr ? rqpair->qpair.ctrlr->cntlid : 0);
		spdk_json_write_named_bool(w, "shadow", rqpair->xrc->is_shadow);
		spdk_json_write_named_uint32(w, "tgt_qpn", rqpair->xrc->tgt_qpn);
		spdk_json_write_named_uint64(w, "commands", rqpair->xrc->cmds);
		spdk_json_write_named_uint32(w, "qd", rqpair->qd);
		spdk_json_write_named_uint32(w, "outstanding", n);
		spdk_json_write_named_uint32(w, "recv_depth", rqpair->current_recv_depth);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

static void
nvmf_rdma_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller *rpoller;

	assert(w != NULL);

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	spdk_json_write_named_uint64(w, "pending_data_buffer", rgroup->stat.pending_data_buffer);
	spdk_json_write_named_uint64(w, "pending_accel_task", rgroup->stat.pending_accel_task);

	spdk_json_write_named_array_begin(w, "devices");

	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name",
					     ibv_get_device_name(rpoller->device->context->device));
		spdk_json_write_named_uint64(w, "polls",
					     rpoller->stat.polls);
		spdk_json_write_named_uint64(w, "idle_polls",
					     rpoller->stat.idle_polls);
		spdk_json_write_named_uint64(w, "completions",
					     rpoller->stat.completions);
		spdk_json_write_named_uint64(w, "requests",
					     rpoller->stat.requests);
		spdk_json_write_named_uint64(w, "request_latency",
					     rpoller->stat.request_latency);
		spdk_json_write_named_uint64(w, "pending_free_request",
					     rpoller->stat.pending_free_request);
		spdk_json_write_named_uint64(w, "pending_rdma_read",
					     rpoller->stat.pending_rdma_read);
		spdk_json_write_named_uint64(w, "pending_rdma_write",
					     rpoller->stat.pending_rdma_write);
		spdk_json_write_named_uint64(w, "pending_rdma_send",
					     rpoller->stat.pending_rdma_send);
		spdk_json_write_named_uint64(w, "total_send_wrs",
					     rpoller->stat.qp_stats.send.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "send_doorbell_updates",
					     rpoller->stat.qp_stats.send.doorbell_updates);
		spdk_json_write_named_uint64(w, "total_recv_wrs",
					     rpoller->stat.qp_stats.recv.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "recv_doorbell_updates",
					     rpoller->stat.qp_stats.recv.doorbell_updates);
		if (rpoller->xrc_srq != NULL) {
			nvmf_rdma_xrc_dump_poller(rpoller, w);
		}
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.name = "RDMA",
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.opts_init = nvmf_rdma_opts_init,
	.create = nvmf_rdma_create,
	.dump_opts = nvmf_rdma_dump_opts,
	.destroy = nvmf_rdma_destroy,

	.listen = nvmf_rdma_listen,
	.stop_listen = nvmf_rdma_stop_listen,
	.cdata_init = nvmf_rdma_cdata_init,

	.listener_discover = nvmf_rdma_discover,

	.poll_group_create = nvmf_rdma_poll_group_create,
	.get_optimal_poll_group = nvmf_rdma_get_optimal_poll_group,
	.poll_group_destroy = nvmf_rdma_poll_group_destroy,
	.poll_group_add = nvmf_rdma_poll_group_add,
	.poll_group_remove = nvmf_rdma_poll_group_remove,
	.poll_group_poll = nvmf_rdma_poll_group_poll,

	.req_free = nvmf_rdma_request_free,
	.req_complete = nvmf_rdma_request_complete,

	.qpair_fini = nvmf_rdma_close_qpair,
	.qpair_get_peer_trid = nvmf_rdma_qpair_get_peer_trid,
	.qpair_get_local_trid = nvmf_rdma_qpair_get_local_trid,
	.qpair_get_listen_trid = nvmf_rdma_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_rdma_qpair_abort_request,

	.poll_group_dump_stat = nvmf_rdma_poll_group_dump_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(rdma, &spdk_nvmf_transport_rdma);
SPDK_LOG_REGISTER_COMPONENT(rdma)

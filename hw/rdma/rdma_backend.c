/*
 * QEMU paravirtual RDMA - Generic RDMA backend
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
 */

#include <qemu/osdep.h>
#include <qemu/error-report.h>
#include <qapi/error.h>

#include <infiniband/verbs.h>

#include "trace.h"
#include "rdma_utils.h"
#include "rdma_rm.h"
#include "rdma_backend.h"

/* Vendor Errors */
#define VENDOR_ERR_FAIL_BACKEND     0x201
#define VENDOR_ERR_TOO_MANY_SGES    0x202
#define VENDOR_ERR_NOMEM            0x203
#define VENDOR_ERR_QP0              0x204
#define VENDOR_ERR_NO_SGE           0x205
#define VENDOR_ERR_MAD_SEND         0x206
#define VENDOR_ERR_INVLKEY          0x207
#define VENDOR_ERR_MR_SMALL         0x208

typedef struct BackendCtx {
    void *up_ctx;
    uint64_t req_id;
    bool is_tx_req;
} BackendCtx;

static void (*comp_handler)(int status, unsigned int vendor_err, void *ctx);

static void dummy_comp_handler(int status, unsigned int vendor_err, void *ctx)
{
    pr_err("No completion handler is registered\n");
}

static void poll_cq(RdmaDeviceResources *rdma_dev_res, struct ibv_cq *ibcq,
                    bool one_poll)
{
    int i, ne;
    BackendCtx *bctx;
    struct ibv_wc wc[2];

    pr_dbg("Entering poll_cq loop on cq %p\n", ibcq);
    do {
        ne = ibv_poll_cq(ibcq, 2, wc);
        if (ne == 0 && one_poll) {
            pr_dbg("CQ is empty\n");
            return;
        }
    } while (ne < 0);

    pr_dbg("Got %d completion(s) from cq %p\n", ne, ibcq);

    for (i = 0; i < ne; i++) {
        pr_dbg("wr_id=0x%lx\n", wc[i].wr_id);
        pr_dbg("status=%d\n", wc[i].status);

        bctx = rdma_rm_get_cqe_ctx(rdma_dev_res, wc[i].wr_id);
        if (unlikely(!bctx)) {
            pr_dbg("Error: Fail to find ctx for req %ld\n", wc[i].wr_id);
            continue;
        }
        pr_dbg("Processing %s CQE\n", bctx->is_tx_req ? "send" : "recv");

        comp_handler(wc[i].status, wc[i].vendor_err, bctx->up_ctx);

        rdma_rm_dealloc_cqe_ctx(rdma_dev_res, wc[i].wr_id);
        free(bctx);
    }
}

static void *comp_handler_thread(void *arg)
{
    RdmaBackendDev *backend_dev = (RdmaBackendDev *)arg;
    int rc;
    struct ibv_cq *ev_cq;
    void *ev_ctx;

    pr_dbg("Starting\n");

    while (backend_dev->comp_thread.run) {
        pr_dbg("Waiting for completion on channel %p\n", backend_dev->channel);
        rc = ibv_get_cq_event(backend_dev->channel, &ev_cq, &ev_ctx);
        pr_dbg("ibv_get_cq_event=%d\n", rc);
        if (unlikely(rc)) {
            pr_dbg("---> ibv_get_cq_event (%d)\n", rc);
            continue;
        }

        if (unlikely(ibv_req_notify_cq(ev_cq, 0))) {
            pr_dbg("---> ibv_req_notify_cq\n");
        }

        poll_cq(backend_dev->rdma_dev_res, ev_cq, false);

        ibv_ack_cq_events(ev_cq, 1);
    }

    pr_dbg("Going down\n");
    /* TODO: Post cqe for all remaining buffs that were posted */

    return NULL;
}

void rdma_backend_register_comp_handler(void (*handler)(int status,
                                        unsigned int vendor_err, void *ctx))
{
    comp_handler = handler;
}

void rdma_backend_unregister_comp_handler(void)
{
    rdma_backend_register_comp_handler(dummy_comp_handler);
}

int rdma_backend_query_port(RdmaBackendDev *backend_dev,
                            struct ibv_port_attr *port_attr)
{
    int rc;

    memset(port_attr, 0, sizeof(*port_attr));

    rc = ibv_query_port(backend_dev->context, backend_dev->port_num, port_attr);
    if (rc) {
        pr_dbg("Error %d from ibv_query_port\n", rc);
        return -EIO;
    }

    return 0;
}

void rdma_backend_poll_cq(RdmaDeviceResources *rdma_dev_res, RdmaBackendCQ *cq)
{
    poll_cq(rdma_dev_res, cq->ibcq, true);
}

static GHashTable *ah_hash;

static struct ibv_ah *create_ah(RdmaBackendDev *backend_dev, struct ibv_pd *pd,
                                uint8_t sgid_idx, union ibv_gid *dgid)
{
    GBytes *ah_key = g_bytes_new(dgid, sizeof(*dgid));
    struct ibv_ah *ah = g_hash_table_lookup(ah_hash, ah_key);

    if (ah) {
        trace_create_ah_cache_hit(be64_to_cpu(dgid->global.subnet_prefix),
                                  be64_to_cpu(dgid->global.interface_id));
        g_bytes_unref(ah_key);
    } else {
        struct ibv_ah_attr ah_attr = {
            .is_global     = 1,
            .port_num      = backend_dev->port_num,
            .grh.hop_limit = 1,
        };

        ah_attr.grh.dgid = *dgid;
        ah_attr.grh.sgid_index = sgid_idx;

        ah = ibv_create_ah(pd, &ah_attr);
        if (ah) {
            g_hash_table_insert(ah_hash, ah_key, ah);
        } else {
            pr_dbg("ibv_create_ah failed for gid <%lx %lx>\n",
                    be64_to_cpu(dgid->global.subnet_prefix),
                    be64_to_cpu(dgid->global.interface_id));
        }

        trace_create_ah_cache_miss(be64_to_cpu(dgid->global.subnet_prefix),
                                   be64_to_cpu(dgid->global.interface_id));
    }

    return ah;
}

static void destroy_ah_hash_key(gpointer data)
{
    g_bytes_unref(data);
}

static void destroy_ah_hast_data(gpointer data)
{
    struct ibv_ah *ah = data;
    ibv_destroy_ah(ah);
}

static void ah_cache_init(void)
{
    ah_hash = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                    destroy_ah_hash_key, destroy_ah_hast_data);
}

static int build_host_sge_array(RdmaDeviceResources *rdma_dev_res,
                                struct ibv_sge *dsge, struct ibv_sge *ssge,
                                uint8_t num_sge)
{
    RdmaRmMR *mr;
    int ssge_idx;
    int ret = 0;

    pr_dbg("num_sge=%d\n", num_sge);

    for (ssge_idx = 0; ssge_idx < num_sge; ssge_idx++) {
        mr = rdma_rm_get_mr(rdma_dev_res, ssge[ssge_idx].lkey);
        if (unlikely(!mr)) {
            ret = VENDOR_ERR_INVLKEY | ssge[ssge_idx].lkey;
            pr_dbg("Invalid lkey %d\n", ssge[ssge_idx].lkey);
            goto out;
        }

        dsge->addr = mr->user_mr.host_virt + ssge[ssge_idx].addr -
                     mr->user_mr.guest_start;
        dsge->length = ssge[ssge_idx].length;
        dsge->lkey = rdma_backend_mr_lkey(&mr->backend_mr);

        pr_dbg("ssge->addr=0x%lx\n", (uint64_t)ssge[ssge_idx].addr);
        pr_dbg("dsge->addr=0x%lx\n", dsge->addr);
        pr_dbg("dsge->lenght=%d\n", dsge->length);
        pr_dbg("dsge->lkey=0x%x\n", dsge->lkey);

        dsge++;
    }

out:
    return ret;
}

void rdma_backend_post_send(RdmaBackendDev *backend_dev,
                            RdmaDeviceResources *rdma_dev_res,
                            RdmaBackendQP *qp, uint8_t qp_type,
                            struct ibv_sge *sge, uint32_t num_sge,
                            union ibv_gid *dgid, uint32_t dqpn,
                            uint32_t dqkey, void *ctx)
{
    BackendCtx *bctx;
    struct ibv_sge new_sge[MAX_SGE];
    uint32_t bctx_id;
    int rc;
    struct ibv_send_wr wr = {0}, *bad_wr;

    if (!qp->ibqp && qp_type == 0) {
        pr_dbg("QP0 is not supported\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_QP0, ctx);
        return;
    }

    pr_dbg("num_sge=%d\n", num_sge);

    if (!qp->ibqp && qp_type == 1) {
        pr_dbg("QP1\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_MAD_SEND, ctx);
    }

    if (!num_sge) {
        pr_dbg("num_sge=0\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_NO_SGE, ctx);
        return;
    }

    bctx = malloc(sizeof(*bctx));
    if (unlikely(!bctx)) {
        pr_dbg("Fail to allocate request ctx\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_NOMEM, ctx);
        return;
    }
    memset(bctx, 0, sizeof(*bctx));

    bctx->up_ctx = ctx;
    bctx->is_tx_req = 1;

    rc = rdma_rm_alloc_cqe_ctx(rdma_dev_res, &bctx_id, bctx);
    if (unlikely(rc)) {
        pr_dbg("Fail to allocate cqe_ctx\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_NOMEM, ctx);
        goto out_free_bctx;
    }

    rc = build_host_sge_array(rdma_dev_res, new_sge, sge, num_sge);
    if (rc) {
        pr_dbg("Error: Fail to build host SGE array\n");
        comp_handler(IBV_WC_GENERAL_ERR, rc, ctx);
        goto out_dealloc_cqe_ctx;
    }

    if (qp_type == IBV_QPT_UD) {
        wr.wr.ud.ah = create_ah(backend_dev, qp->ibpd,
                                backend_dev->backend_gid_idx, dgid);
        wr.wr.ud.remote_qpn = dqpn;
        wr.wr.ud.remote_qkey = dqkey;
    }

    wr.num_sge = num_sge;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.sg_list = &new_sge[0];
    wr.wr_id = bctx_id;

    rc = ibv_post_send(qp->ibqp, &wr, &bad_wr);
    pr_dbg("ibv_post_send=%d\n", rc);
    if (rc) {
        pr_dbg("Fail (%d, %d) to post send WQE to qpn %d\n", rc, errno,
                qp->ibqp->qp_num);
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_FAIL_BACKEND, ctx);
        goto out_dealloc_cqe_ctx;
    }

    return;

out_dealloc_cqe_ctx:
    rdma_rm_dealloc_cqe_ctx(rdma_dev_res, bctx_id);

out_free_bctx:
    free(bctx);
}

void rdma_backend_post_recv(RdmaBackendDev *backend_dev,
                            RdmaDeviceResources *rdma_dev_res,
                            RdmaBackendQP *qp, uint8_t qp_type,
                            struct ibv_sge *sge, uint32_t num_sge, void *ctx)
{
    BackendCtx *bctx;
    struct ibv_sge new_sge[MAX_SGE];
    uint32_t bctx_id;
    int rc;
    struct ibv_recv_wr wr = {0}, *bad_wr;

    if (!qp->ibqp && qp_type == 0) {
        pr_dbg("QP0 is not supported\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_QP0, ctx);
        return;
    }

    pr_dbg("num_sge=%d\n", num_sge);

    if (!qp->ibqp && qp_type == 1) {
        pr_dbg("QP1\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_MAD_SEND, ctx);
        return;
    }

    if (!num_sge) {
        pr_dbg("num_sge=0\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_NO_SGE, ctx);
        return;
    }

    bctx = malloc(sizeof(*bctx));
    if (unlikely(!bctx)) {
        pr_dbg("Fail to allocate request ctx\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_NOMEM, ctx);
        return;
    }
    memset(bctx, 0, sizeof(*bctx));

    bctx->up_ctx = ctx;
    bctx->is_tx_req = 0;

    rc = rdma_rm_alloc_cqe_ctx(rdma_dev_res, &bctx_id, bctx);
    if (unlikely(rc)) {
        pr_dbg("Fail to allocate cqe_ctx\n");
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_NOMEM, ctx);
        goto out_free_bctx;
    }

    rc = build_host_sge_array(rdma_dev_res, new_sge, sge, num_sge);
    if (rc) {
        pr_dbg("Error: Fail to build host SGE array\n");
        comp_handler(IBV_WC_GENERAL_ERR, rc, ctx);
        goto out_dealloc_cqe_ctx;
    }

    wr.num_sge = num_sge;
    wr.sg_list = &new_sge[0];
    wr.wr_id = bctx_id;
    rc = ibv_post_recv(qp->ibqp, &wr, &bad_wr);
    pr_dbg("ibv_post_recv=%d\n", rc);
    if (rc) {
        pr_dbg("Fail (%d, %d) to post recv WQE to qpn %d\n", rc, errno,
                qp->ibqp->qp_num);
        comp_handler(IBV_WC_GENERAL_ERR, VENDOR_ERR_FAIL_BACKEND, ctx);
        goto out_dealloc_cqe_ctx;
    }

    return;

out_dealloc_cqe_ctx:
    rdma_rm_dealloc_cqe_ctx(rdma_dev_res, bctx_id);

out_free_bctx:
    free(bctx);
}

int rdma_backend_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    pd->ibpd = ibv_alloc_pd(backend_dev->context);

    return pd->ibpd ? 0 : -EIO;
}

void rdma_backend_destroy_pd(RdmaBackendPD *pd)
{
    if (pd->ibpd) {
        ibv_dealloc_pd(pd->ibpd);
    }
}

int rdma_backend_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, uint64_t addr,
                           size_t length, int access)
{
    pr_dbg("addr=0x%lx\n", addr);
    pr_dbg("len=%ld\n", length);
    mr->ibpd = pd->ibpd;
    mr->ibmr = ibv_reg_mr(mr->ibpd, (void *)addr, length, access);

    if (mr->ibmr) {
        pr_dbg("lkey=0x%x\n", mr->ibmr->lkey);
        pr_dbg("rkey=0x%x\n", mr->ibmr->rkey);
    }

    return mr->ibmr ? 0 : -EIO;
}

void rdma_backend_destroy_mr(RdmaBackendMR *mr)
{
    if (mr->ibmr) {
        ibv_dereg_mr(mr->ibmr);
    }
}

int rdma_backend_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                           int cqe)
{
    pr_dbg("cqe=%d\n", cqe);

    pr_dbg("dev->channel=%p\n", backend_dev->channel);
    cq->ibcq = ibv_create_cq(backend_dev->context, cqe + 1, NULL,
                             backend_dev->channel, 0);

    if (cq->ibcq) {
        if (ibv_req_notify_cq(cq->ibcq, 0)) {
            pr_dbg("---> ibv_req_notify_cq\n");
        }
    }

    cq->backend_dev = backend_dev;

    return cq->ibcq ? 0 : -EIO;
}

void rdma_backend_destroy_cq(RdmaBackendCQ *cq)
{
    if (cq->ibcq) {
        ibv_req_notify_cq(cq->ibcq, 0);

        /* Cleanup the queue before destruction */
        poll_cq(cq->backend_dev->rdma_dev_res, cq->ibcq, false);

        ibv_destroy_cq(cq->ibcq);
    }
}

int rdma_backend_create_qp(RdmaBackendQP *qp, uint8_t qp_type,
                           RdmaBackendPD *pd, RdmaBackendCQ *scq,
                           RdmaBackendCQ *rcq, uint32_t max_send_wr,
                           uint32_t max_recv_wr, uint32_t max_send_sge,
                           uint32_t max_recv_sge)
{
    struct ibv_qp_init_attr attr = {0};

    qp->ibqp = 0;
    pr_dbg("qp_type=%d\n", qp_type);

    if (qp_type == 0) {
        pr_dbg("QP0 is not supported\n");
        return -EPERM;
    }

    if (qp_type == 1) {
        pr_dbg("QP1\n");
        return 0;
    }

    attr.qp_type = qp_type;
    attr.send_cq = scq->ibcq;
    attr.recv_cq = rcq->ibcq;
    attr.cap.max_send_wr = max_send_wr;
    attr.cap.max_recv_wr = max_recv_wr;
    attr.cap.max_send_sge = max_send_sge;
    attr.cap.max_recv_sge = max_recv_sge;

    pr_dbg("max_send_wr=%d\n", max_send_wr);
    pr_dbg("max_recv_wr=%d\n", max_recv_wr);
    pr_dbg("max_send_sge=%d\n", max_send_sge);
    pr_dbg("max_recv_sge=%d\n", max_recv_sge);

    qp->ibpd = pd->ibpd;
    qp->ibqp = ibv_create_qp(qp->ibpd, &attr);

    if (likely(!qp->ibqp)) {
        pr_dbg("Error from ibv_create_qp\n");
        return -EIO;
    }

    /* TODO: Query QP to get max_inline_data and save it to be used in send */

    pr_dbg("qpn=0x%x\n", qp->ibqp->qp_num);

    return 0;
}

int rdma_backend_qp_state_init(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                               uint8_t qp_type, uint32_t qkey)
{
    struct ibv_qp_attr attr = {0};
    int rc, attr_mask;

    pr_dbg("qpn=0x%x\n", qp->ibqp->qp_num);
    pr_dbg("sport_num=%d\n", backend_dev->port_num);

    attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = backend_dev->port_num;
    if (qp_type == IBV_QPT_RC) {
        attr_mask |= IBV_QP_ACCESS_FLAGS;
    }
    if (qp_type == IBV_QPT_UD) {
        attr.qkey = qkey;
        if (qkey) {
            attr_mask |= IBV_QP_QKEY;
        }
    }

    rc = ibv_modify_qp(qp->ibqp, &attr, attr_mask);
    if (rc) {
        pr_dbg("Error %d from ibv_modify_qp\n", rc);
        return -EIO;
    }

    return 0;
}

int rdma_backend_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                              uint8_t qp_type, union ibv_gid *dgid,
                              uint32_t dqpn, uint32_t rq_psn, uint32_t qkey)
{
    struct ibv_qp_attr attr = {0};
    union ibv_gid ibv_gid = {
        .global.interface_id = dgid->global.interface_id,
        .global.subnet_prefix = dgid->global.subnet_prefix
    };
    int rc, attr_mask = 0;

    attr.qp_state = IBV_QPS_RTR;
    attr_mask = IBV_QP_STATE;

    if (qp_type == IBV_QPT_RC) {
        pr_dbg("dgid=0x%lx,%lx\n", be64_to_cpu(ibv_gid.global.subnet_prefix),
               be64_to_cpu(ibv_gid.global.interface_id));
        pr_dbg("dqpn=0x%x\n", dqpn);
        pr_dbg("sgid_idx=%d\n", backend_dev->backend_gid_idx);
        pr_dbg("sport_num=%d\n", backend_dev->port_num);
        pr_dbg("rq_psn=0x%x\n", rq_psn);

        attr.path_mtu               = IBV_MTU_1024;
        attr.dest_qp_num            = dqpn;
        attr.max_dest_rd_atomic     = 1;
        attr.min_rnr_timer          = 12;
        attr.ah_attr.port_num       = backend_dev->port_num;
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.hop_limit  = 1;
        attr.ah_attr.grh.dgid       = ibv_gid;
        attr.ah_attr.grh.sgid_index = backend_dev->backend_gid_idx;
        attr.rq_psn                 = rq_psn;

        attr_mask |= IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                     IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                     IBV_QP_MIN_RNR_TIMER;
    }

    if (qp_type == IBV_QPT_UD) {
        pr_dbg("qkey=0x%x\n", qkey);
        attr.qkey = qkey;
        if (qkey) {
            attr_mask |= IBV_QP_QKEY;
        }
    }

    rc = ibv_modify_qp(qp->ibqp, &attr, attr_mask);
    if (rc) {
        pr_dbg("Error %d from ibv_modify_qp\n", rc);
        return -EIO;
    }

    return 0;
}

int rdma_backend_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type,
                              uint32_t sq_psn, uint32_t qkey)
{
    struct ibv_qp_attr attr = {0};
    int rc, attr_mask = 0;

    pr_dbg("qpn=0x%x\n", qp->ibqp->qp_num);
    pr_dbg("sq_psn=0x%x\n", sq_psn);

    attr_mask |= IBV_QP_SQ_PSN;
    attr.sq_psn = sq_psn;

    attr.qp_state = IBV_QPS_RTS;
    attr_mask = IBV_QP_STATE | IBV_QP_SQ_PSN;

    if (qp_type == IBV_QPT_RC) {
        attr.timeout       = 14;
        attr.retry_cnt     = 7;
        attr.rnr_retry     = 7;
        attr.max_rd_atomic = 1;

        attr_mask |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                     IBV_QP_MAX_QP_RD_ATOMIC;
    }

    if (qp_type == IBV_QPT_UD) {
        attr.qkey = qkey;
        if (qkey) {
            attr_mask |= IBV_QP_QKEY;
        }
    }

    rc = ibv_modify_qp(qp->ibqp, &attr, attr_mask);
    if (rc) {
        pr_dbg("Error %d from ibv_modify_qp\n", rc);
        return -EIO;
    }

    return 0;
}

void rdma_backend_destroy_qp(RdmaBackendQP *qp)
{
    if (qp->ibqp) {
        ibv_destroy_qp(qp->ibqp);
    }
}

#define CHK_ATTR(req, dev, member, fmt) ({ \
    pr_dbg("%s="fmt","fmt"\n", #member, dev.member, req->member); \
    if (req->member > dev.member) { \
        warn_report("Setting of %s to 0x%lx higher than host device capability 0x%lx", \
                    #member, (uint64_t)req->member, (uint64_t)dev.member); \
        req->member = dev.member; \
    } \
    pr_dbg("%s="fmt"\n", #member, req->member); })

static int init_device_caps(RdmaBackendDev *backend_dev,
                            struct ibv_device_attr *dev_attr)
{
    memset(&backend_dev->dev_attr, 0, sizeof(backend_dev->dev_attr));

    if (ibv_query_device(backend_dev->context, &backend_dev->dev_attr)) {
        return -EIO;
    }

    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_mr_size, "%ld");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_qp, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_sge, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_qp_wr, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_cq, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_cqe, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_mr, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_pd, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_qp_rd_atom, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_qp_init_rd_atom, "%d");
    CHK_ATTR(dev_attr, backend_dev->dev_attr, max_ah, "%d");

    return 0;
}

int rdma_backend_init(RdmaBackendDev *backend_dev,
                      RdmaDeviceResources *rdma_dev_res,
                      const char *backend_device_name, uint8_t port_num,
                      uint8_t backend_gid_idx, struct ibv_device_attr *dev_attr,
                      Error **errp)
{
    int i;
    int ret = 0;
    int num_ibv_devices;
    char thread_name[80] = {0};
    struct ibv_device **dev_list;
    struct ibv_port_attr port_attr;

    backend_dev->backend_gid_idx = backend_gid_idx;
    backend_dev->port_num = port_num;
    backend_dev->rdma_dev_res = rdma_dev_res;

    rdma_backend_register_comp_handler(dummy_comp_handler);

    dev_list = ibv_get_device_list(&num_ibv_devices);
    if (!dev_list) {
        error_setg(errp, "Failed to get IB devices list");
        ret = -EIO;
        goto out;
    }
    if (num_ibv_devices == 0) {
        error_setg(errp, "No IB devices were found");
        ret = -ENXIO;
        goto out;
    }

    if (backend_device_name) {
        for (i = 0; dev_list[i]; ++i) {
            if (!strcmp(ibv_get_device_name(dev_list[i]),
                        backend_device_name)) {
                break;
            }
        }

        backend_dev->ib_dev = dev_list[i];
        if (!backend_dev->ib_dev) {
            error_setg(errp, "Failed to find IB device %s",
                       backend_device_name);
            ret = -EIO;
            goto out;
        }
    } else {
        backend_dev->ib_dev = *dev_list;
    }
    ibv_free_device_list(dev_list);

    pr_dbg("Using backend device %s, port %d, gid_idx %d\n",
           ibv_get_device_name(backend_dev->ib_dev),
           backend_dev->port_num, backend_dev->backend_gid_idx);

    backend_dev->context = ibv_open_device(backend_dev->ib_dev);
    if (!backend_dev->context) {
        error_setg(errp, "Failed to open IB device");
        ret = -EIO;
        goto out;
    }

    backend_dev->channel = ibv_create_comp_channel(backend_dev->context);
    if (!backend_dev->channel) {
        error_setg(errp, "Failed to create IB communication channel");
        ret = -EIO;
        goto out_close_device;
    }
    pr_dbg("dev->backend_dev.channel=%p\n", backend_dev->channel);

    ret = ibv_query_gid(backend_dev->context, backend_dev->port_num,
                         backend_dev->backend_gid_idx, &backend_dev->gid);
    if (ret) {
        error_setg(errp, "Failed to query gid %d",
                   backend_dev->backend_gid_idx);
        ret = -EIO;
        goto out_destroy_comm_channel;
    }
    pr_dbg("subnet_prefix=0x%lx\n",
           be64_to_cpu(backend_dev->gid.global.subnet_prefix));
    pr_dbg("interface_id=0x%lx\n",
           be64_to_cpu(backend_dev->gid.global.interface_id));

    ret = ibv_query_port(backend_dev->context, backend_dev->port_num,
                         &port_attr);
    if (ret) {
        error_setg(errp, "Error %d from ibv_query_port", ret);
        ret = -EIO;
        goto out_destroy_comm_channel;
    }

    ret = init_device_caps(backend_dev, dev_attr);
    if (ret) {
        error_setg(errp, "Fail to initialize device capabilities");
        ret = -EIO;
        goto out_destroy_comm_channel;
    }

    sprintf(thread_name, "pvrdma_comp_%s",
            ibv_get_device_name(backend_dev->ib_dev));
    backend_dev->comp_thread.run = true;
    qemu_thread_create(&backend_dev->comp_thread.thread, thread_name,
                       comp_handler_thread, backend_dev, QEMU_THREAD_DETACHED);

    ah_cache_init();

    goto out;

out_destroy_comm_channel:
    ibv_destroy_comp_channel(backend_dev->channel);

out_close_device:
    ibv_close_device(backend_dev->context);

out:
    return ret;
}

void rdma_backend_fini(RdmaBackendDev *backend_dev)
{
    g_hash_table_destroy(ah_hash);
    ibv_destroy_comp_channel(backend_dev->channel);
    ibv_close_device(backend_dev->context);
}

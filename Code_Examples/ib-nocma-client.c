#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <netdb.h>
#include <errno.h>
#include <assert.h>
#include <infiniband/verbs.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define DHT_SERVER_PORT (18000)
#define DHT_RX_DEPTH    (500)
#define DHT_MESG_SIZE   (4096)
#define DHT_IB_PORT     (1)
#define DHT_MTU         IBV_MTU_1024
#define DHT_SERV_LEVEL  (0)

#define DHT_RECV_WRID (1)
#define DHT_SEND_WRID (2)
#define DHT_NEW_SEND_API 0

int cn_get_port_info(struct ibv_context *context, int port,
		     struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

static int page_size;

struct conn_context {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_dm *dm;
    struct ibv_cq *cq;
    struct ibv_qp *qp;

    struct ibv_qp_ex *qpx;
    char *buf;
    int size;
    int send_flags;
    int rx_depth;
    int pending;
    struct ibv_port_attr portinfo;
};


struct conn_info {
    int lid;
    int qpn;
    int psn;
    uint32_t rkey;
    uint64_t addr;
};

static struct conn_context *cn_init_ctx(struct ibv_device *ib_dev,
					int port)
{
    struct conn_context *ctx;
    int access_flags = IBV_ACCESS_LOCAL_WRITE;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
	return NULL;

    ctx->size = DHT_MESG_SIZE;
    ctx->send_flags = IBV_SEND_SIGNALED;
    ctx->rx_depth = DHT_RX_DEPTH;

    ctx->buf = memalign(page_size, ctx->size);
    if (!ctx->buf) {
	fprintf(stderr, "Couldn't allocate work buf.\n");
	goto clean_ctx;
    }

    memset(ctx->buf, 0x7b, ctx->size);
    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
	fprintf(stderr, "Couldn't get context for %s\n",
		ibv_get_device_name(ib_dev));
	goto clean_buffer;
    }

    ctx->channel = ibv_create_comp_channel(ctx->context);
    if (!ctx->channel) {
	fprintf(stderr, "Couldn't create completion channel\n");
	goto clean_device;
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
	fprintf(stderr, "Couldn't allocate PD\n");
	goto clean_comp_channel;
    }


    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size, access_flags);
    if (!ctx->mr) {
	fprintf(stderr, "Couldn't register MR\n");
	goto clean_pd;
    }

    ctx->cq =
	ibv_create_cq(ctx->context, DHT_RX_DEPTH + 1, NULL, ctx->channel,
		      0);
    if (!ctx->cq) {
	fprintf(stderr, "Couldn't create CQ\n");
	goto clean_mr;
    }

    {
	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr = {
	    .send_cq = (ctx->cq),
	    .recv_cq = (ctx->cq),
	    .cap = {
		    .max_send_wr = 1,
		    .max_recv_wr = DHT_RX_DEPTH,
		    .max_send_sge = 1,
		    .max_recv_sge = 1 },
	    .qp_type = IBV_QPT_RC
	};

	ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
	if (!ctx->qp) {
	    fprintf(stderr, "Couldn't create QP\n");
	    goto clean_cq;
	}

	ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
	if (init_attr.cap.max_inline_data >= ctx->size)
	    ctx->send_flags |= IBV_SEND_INLINE;
    }

    {
	struct ibv_qp_attr attr = {
	    .qp_state = IBV_QPS_INIT,
	    .pkey_index = 0,
	    .port_num = port,
	    .qp_access_flags = 0
	};

	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE |
			  IBV_QP_PKEY_INDEX |
			  IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
	    fprintf(stderr, "Failed to modify QP to INIT\n");
	    goto clean_qp;
	}
    }

    return ctx;

  clean_qp:
    ibv_destroy_qp(ctx->qp);

  clean_cq:
    ibv_destroy_cq(ctx->cq);

  clean_mr:
    ibv_dereg_mr(ctx->mr);

  clean_pd:
    ibv_dealloc_pd(ctx->pd);

  clean_comp_channel:
    if (ctx->channel)
	ibv_destroy_comp_channel(ctx->channel);

  clean_device:
    ibv_close_device(ctx->context);

  clean_buffer:
    free(ctx->buf);

  clean_ctx:
    free(ctx);

    return NULL;
}


static int cn_close_ctx(struct conn_context *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
	fprintf(stderr, "Couldn't destroy QP\n");
	return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
	fprintf(stderr, "Couldn't destroy CQ\n");
	return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
	fprintf(stderr, "Couldn't deregister MR\n");
	return 1;
    }

    if (ctx->dm) {
	if (ibv_free_dm(ctx->dm)) {
	    fprintf(stderr, "Couldn't free DM\n");
	    return 1;
	}
    }

    if (ibv_dealloc_pd(ctx->pd)) {
	fprintf(stderr, "Couldn't deallocate PD\n");
	return 1;
    }

    if (ctx->channel) {
	if (ibv_destroy_comp_channel(ctx->channel)) {
	    fprintf(stderr, "Couldn't destroy completion channel\n");
	    return 1;
	}
    }

    if (ibv_close_device(ctx->context)) {
	fprintf(stderr, "Couldn't release context\n");
	return 1;
    }

    free(ctx->buf);
    free(ctx);

    return 0;
}

static int cn_post_recv(struct conn_context *ctx, int n)
{
    struct ibv_sge list = {
	.addr = (uintptr_t) ctx->buf,
	.length = ctx->size,
	.lkey = ctx->mr->lkey
    };
    struct ibv_recv_wr wr = {
	.wr_id = DHT_RECV_WRID,
	.sg_list = &list,
	.num_sge = 1,
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
	if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
	    break;

    return i;
}


static int cn_post_send(struct conn_context *ctx)
{
    struct ibv_sge list = {
	.addr = (uintptr_t) ctx->buf,
	.length = ctx->size,
	.lkey = ctx->mr->lkey
    };
    struct ibv_send_wr wr = {
	.wr_id = DHT_SEND_WRID,
	.sg_list = &list,
	.num_sge = 1,
	.opcode = IBV_WR_SEND,
	.send_flags = ctx->send_flags,
    };
    struct ibv_send_wr *bad_wr;

    if (DHT_NEW_SEND_API) {
	ibv_wr_start(ctx->qpx);

	ctx->qpx->wr_id = DHT_SEND_WRID;
	ctx->qpx->wr_flags = ctx->send_flags;

	ibv_wr_send(ctx->qpx);
	ibv_wr_set_sge(ctx->qpx, list.lkey, list.addr, list.length);

	return ibv_wr_complete(ctx->qpx);
    } else {
	return ibv_post_send(ctx->qp, &wr, &bad_wr);
    }
}

static int cn_connect_ctx(struct conn_context *ctx, int port, int my_psn,
			  struct conn_info *dest)
{
    struct ibv_qp_attr attr = {
	.qp_state = IBV_QPS_RTR,
	.path_mtu = DHT_MTU,
	.dest_qp_num = dest->qpn,
	.rq_psn = dest->psn,
	.max_dest_rd_atomic = 1,
	.min_rnr_timer = 12,
	.ah_attr = {
		    .is_global = 0,
		    .dlid = dest->lid,
		    .sl = DHT_SERV_LEVEL,
		    .src_path_bits = 0,
		    .port_num = port }
    };

    if (ibv_modify_qp(ctx->qp, &attr,
		      IBV_QP_STATE |
		      IBV_QP_AV |
		      IBV_QP_PATH_MTU |
		      IBV_QP_DEST_QPN |
		      IBV_QP_RQ_PSN |
		      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
	fprintf(stderr, "Failed to modify QP to RTR\n");
	return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;

    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = my_psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(ctx->qp, &attr,
		      IBV_QP_STATE |
		      IBV_QP_TIMEOUT |
		      IBV_QP_RETRY_CNT |
		      IBV_QP_RNR_RETRY |
		      IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
	fprintf(stderr, "Failed to modify QP to RTS\n");
	return 1;
    }

    return 0;
}

static int poll_rdma_completion(struct conn_context *ctx, int rw)
{
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    int ret;
    int flushed = 0;

    while ((ret = ibv_poll_cq(ctx->cq, 1, &wc)) == 1) {
	ret = 0;
	if (wc.status) {
	    if (wc.status == IBV_WC_WR_FLUSH_ERR) {
		flushed = 1;
		continue;

	    }
	    fprintf(stderr, "cq completion failed status %d\n", wc.status);
	    ret = -1;
	    goto error;
	}

	switch (wc.opcode) {

	case IBV_WC_RDMA_WRITE:
	    if (rw == IBV_WC_RDMA_WRITE)
		return 0;
	    break;

	case IBV_WC_RDMA_READ:
	    if (rw == IBV_WC_RDMA_READ)
		return 0;
	    break;

	default:
	    fprintf(stderr, "unexpected opcode completion\n");
	    ret = -1;
	    goto error;
	}
    }

    if (ret) {
	fprintf(stderr, "poll error %d\n", ret);
	goto error;
    }
    return flushed;

  error:
    return ret;
}



int main(int argc, char **argv)
{
    char hostname[256];
    char hosts[2][256];

    int server_port = DHT_SERVER_PORT;

    struct conn_context *ctx;
    struct conn_info my_info, peer_info;

    if (argc != 3) {
	printf("Client usage: %s <server> <port_greater_than_18000>\n",
	       argv[0]);
	return 1;
    }

    server_port = atoi(argv[2]);
    gethostname(hostname, 256);

    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;

    page_size = sysconf(_SC_PAGESIZE);
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
	perror("Failed to get IB devices list");
	return 1;
    }
    ib_dev = *dev_list;

    ctx = cn_init_ctx(ib_dev, DHT_IB_PORT);
    if (!ctx) {
	return 1;
    }

    int routs = cn_post_recv(ctx, ctx->rx_depth);
    if (routs < ctx->rx_depth) {
	fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	return 1;
    }

    if (ibv_req_notify_cq(ctx->cq, 0)) {
	fprintf(stderr, "Couldn't request CQ notification\n");
	return 1;
    }

    if (cn_get_port_info(ctx->context, DHT_IB_PORT, &ctx->portinfo)) {
	fprintf(stderr, "Couldn't get port info\n");
	return 1;
    }

    my_info.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
	!my_info.lid) {
	fprintf(stderr, "Couldn't get local LID\n");
	return 1;
    }

    my_info.qpn = ctx->qp->qp_num;
    my_info.psn = lrand48() & 0xffffff;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    struct hostent *host_entry = gethostbyname(argv[1]);
    char *ipaddr =
	inet_ntoa(*((struct in_addr *) host_entry->h_addr_list[0]));
    if (inet_pton(AF_INET, ipaddr, &server_addr.sin_addr) <= 0) {
	fprintf(stderr, "\nInvalid address %s Address not supported \n",
		argv[1]);
	return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int ret = connect(sock, (struct sockaddr *) &server_addr,
		      sizeof(server_addr));
    if (ret) {
	fprintf(stderr, "%d: failed to connect %s at port %d\n", __LINE__,
		argv[1], server_port);
    }

    char *shared_buffer = malloc(DHT_MESG_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(ctx->pd, shared_buffer, DHT_MESG_SIZE,
				   IBV_ACCESS_LOCAL_WRITE |
				   IBV_ACCESS_REMOTE_WRITE |
				   IBV_ACCESS_REMOTE_READ |
				   IBV_ACCESS_REMOTE_ATOMIC);
    my_info.rkey = mr->rkey;
    my_info.addr = (uint64_t) mr->addr;

    // exchange (lid, qp number, and psn) to connect QP
    recv(sock, &peer_info, sizeof(peer_info), 0);
    send(sock, &my_info, sizeof(my_info), 0);

    if (cn_connect_ctx(ctx, DHT_IB_PORT, my_info.psn, &peer_info))
	return 1;

    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;
    uint32_t peer_key = peer_info.rkey;
    uint64_t peer_addr = peer_info.addr;

    strcpy(shared_buffer, "Hello from client!");
    memset(&wr, 0, sizeof(wr));
    sge.addr = (uint64_t) shared_buffer;
    sge.length = DHT_MESG_SIZE;
    sge.lkey = mr->lkey;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;	// or remote read

    wr.wr.rdma.remote_addr = peer_addr;
    wr.wr.rdma.rkey = peer_key;
    ibv_post_send(ctx->qp, &wr, &bad_wr);
    poll_rdma_completion(ctx, IBV_WR_RDMA_WRITE);

    // Inform the server of completion
    send(sock, "done", sizeof("done"), 0);

    // deregister the extra mr
    ibv_dereg_mr(mr);

    // Cleanup connection
    cn_close_ctx(ctx);
    fprintf(stderr, "%d: closed QP.\n", __LINE__);
    close(sock);
    return 0;
}

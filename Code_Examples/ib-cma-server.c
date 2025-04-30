#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <errno.h>
#include <assert.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>

static const char *server = "0.0.0.0";

static struct rdma_cm_id *listen_id, *id;
static struct ibv_mr *mr, *send_mr;
static int send_flags;
static char send_msg[16] = "COP5611";
static char recv_msg[16];

static inline int rdma_seterrno(int ret)
{
    if (ret) {
	errno = ret;
	ret = -1;
    }
    return ret;
}

/*
 * Memory registration helpers.
 */
static inline struct ibv_mr *rdma_reg_msgs(struct rdma_cm_id *id,
					   void *addr, size_t length)
{
    return ibv_reg_mr(id->pd, addr, length, IBV_ACCESS_LOCAL_WRITE);
}

static inline struct ibv_mr *rdma_reg_read(struct rdma_cm_id *id,
					   void *addr, size_t length)
{
    return ibv_reg_mr(id->pd, addr, length, IBV_ACCESS_LOCAL_WRITE |
		      IBV_ACCESS_REMOTE_READ);
}

static inline struct ibv_mr *rdma_reg_write(struct rdma_cm_id *id,
					    void *addr, size_t length)
{
    return ibv_reg_mr(id->pd, addr, length, IBV_ACCESS_LOCAL_WRITE |
		      IBV_ACCESS_REMOTE_WRITE);
}

static inline int rdma_dereg_mr(struct ibv_mr *mr)
{
    return rdma_seterrno(ibv_dereg_mr(mr));
}


/*
 * Vectored send, receive, and RDMA operations.
 * Support multiple scatter-gather entries.
 */
static inline int
rdma_post_recvv(struct rdma_cm_id *id, void *context, struct ibv_sge *sgl,
		int nsge)
{
    struct ibv_recv_wr wr, *bad;

    wr.wr_id = (uintptr_t) context;
    wr.next = NULL;
    wr.sg_list = sgl;
    wr.num_sge = nsge;

    if (id->srq)
	return rdma_seterrno(ibv_post_srq_recv(id->srq, &wr, &bad));
    else
	return rdma_seterrno(ibv_post_recv(id->qp, &wr, &bad));
}

static inline int
rdma_post_sendv(struct rdma_cm_id *id, void *context, struct ibv_sge *sgl,
		int nsge, int flags)
{
    struct ibv_send_wr wr, *bad;

    wr.wr_id = (uintptr_t) context;
    wr.next = NULL;
    wr.sg_list = sgl;
    wr.num_sge = nsge;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = flags;

    return rdma_seterrno(ibv_post_send(id->qp, &wr, &bad));
}


static inline int
rdma_post_readv(struct rdma_cm_id *id, void *context, struct ibv_sge *sgl,
		int nsge, int flags, uint64_t remote_addr, uint32_t rkey)
{
    struct ibv_send_wr wr, *bad;

    wr.wr_id = (uintptr_t) context;
    wr.next = NULL;
    wr.sg_list = sgl;
    wr.num_sge = nsge;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = flags;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    return rdma_seterrno(ibv_post_send(id->qp, &wr, &bad));
}

static inline int
rdma_post_writev(struct rdma_cm_id *id, void *context, struct ibv_sge *sgl,
		 int nsge, int flags, uint64_t remote_addr, uint32_t rkey)
{
    struct ibv_send_wr wr, *bad;

    wr.wr_id = (uintptr_t) context;
    wr.next = NULL;
    wr.sg_list = sgl;
    wr.num_sge = nsge;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = flags;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    return rdma_seterrno(ibv_post_send(id->qp, &wr, &bad));
}

/*
 * Simple send, receive, and RDMA calls.
 */
static inline int
rdma_post_recv(struct rdma_cm_id *id, void *context, void *addr,
	       size_t length, struct ibv_mr *mr)
{
    struct ibv_sge sge;

    assert((addr >= mr->addr) &&
	   (((uint8_t *) addr + length) <= ((uint8_t *) mr->addr +
					    mr->length)));
    sge.addr = (uint64_t) (uintptr_t) addr;
    sge.length = (uint32_t) length;
    sge.lkey = mr->lkey;

    return rdma_post_recvv(id, context, &sge, 1);
}

static inline int
rdma_post_send(struct rdma_cm_id *id, void *context, void *addr,
	       size_t length, struct ibv_mr *mr, int flags)
{
    struct ibv_sge sge;

    sge.addr = (uint64_t) (uintptr_t) addr;
    sge.length = (uint32_t) length;
    sge.lkey = mr ? mr->lkey : 0;

    return rdma_post_sendv(id, context, &sge, 1, flags);
}


static inline int
rdma_post_read(struct rdma_cm_id *id, void *context, void *addr,
	       size_t length, struct ibv_mr *mr, int flags,
	       uint64_t remote_addr, uint32_t rkey)
{
    struct ibv_sge sge;

    sge.addr = (uint64_t) (uintptr_t) addr;
    sge.length = (uint32_t) length;
    sge.lkey = mr->lkey;

    return rdma_post_readv(id, context, &sge, 1, flags, remote_addr, rkey);
}

static inline int
rdma_post_write(struct rdma_cm_id *id, void *context, void *addr,
		size_t length, struct ibv_mr *mr, int flags,
		uint64_t remote_addr, uint32_t rkey)
{
    struct ibv_sge sge;

    sge.addr = (uint64_t) (uintptr_t) addr;
    sge.length = (uint32_t) length;
    sge.lkey = mr ? mr->lkey : 0;

    return rdma_post_writev(id, context, &sge, 1, flags, remote_addr,
			    rkey);
}



static inline int
rdma_get_send_comp(struct rdma_cm_id *id, struct ibv_wc *wc)
{
    struct ibv_cq *cq;
    void *context;
    int ret;

    do {
	ret = ibv_poll_cq(id->send_cq, 1, wc);
	if (ret)
	    break;

	ret = ibv_req_notify_cq(id->send_cq, 0);
	if (ret)
	    return rdma_seterrno(ret);

	ret = ibv_poll_cq(id->send_cq, 1, wc);
	if (ret)
	    break;

	ret = ibv_get_cq_event(id->send_cq_channel, &cq, &context);
	if (ret)
	    return ret;

	assert(cq == id->send_cq && context == id);
	ibv_ack_cq_events(id->send_cq, 1);
    } while (1);

    return (ret < 0) ? rdma_seterrno(ret) : ret;
}

static inline int
rdma_get_recv_comp(struct rdma_cm_id *id, struct ibv_wc *wc)
{
    struct ibv_cq *cq;
    void *context;
    int ret;

    do {
	ret = ibv_poll_cq(id->recv_cq, 1, wc);
	if (ret)
	    break;

	ret = ibv_req_notify_cq(id->recv_cq, 0);
	if (ret)
	    return rdma_seterrno(ret);

	ret = ibv_poll_cq(id->recv_cq, 1, wc);
	if (ret)
	    break;

	ret = ibv_get_cq_event(id->recv_cq_channel, &cq, &context);
	if (ret)
	    return ret;

	assert(cq == id->recv_cq && context == id);
	ibv_ack_cq_events(id->recv_cq, 1);
    } while (1);

    return (ret < 0) ? rdma_seterrno(ret) : ret;
}


static int run_server(const char* ipaddr, const int port)
{
    struct rdma_addrinfo hints, *res;
    struct ibv_qp_init_attr init_attr;
    struct ibv_qp_attr qp_attr;
    struct ibv_wc wc;
    int ret;
    char port_id[64];
    sprintf(port_id, "%d", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_flags = RAI_PASSIVE;
    hints.ai_port_space = RDMA_PS_TCP;
    ret = rdma_getaddrinfo(server, port_id, &hints, &res);
    //ret = rdma_getaddrinfo(ipaddr, port_id, NULL, &res);
    if (ret) {
	printf("%d rdma_getaddrinfo: %d, %s \n", __LINE__, errno, gai_strerror(ret));
	return ret;
    }

    memset(&init_attr, 0, sizeof init_attr);
    init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = 1;
    init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = 16;
    init_attr.sq_sig_all = 1;

    ret = rdma_create_ep(&listen_id, res, NULL, &init_attr);
    if (ret) {
	perror("rdma_create_ep");
	goto out_free_addrinfo;
    }

    ret = rdma_listen(listen_id, 0);
    if (ret) {
	perror("rdma_listen");
	goto out_destroy_listen_ep;
    }

    ret = rdma_get_request(listen_id, &id);
    if (ret) {
	perror("rdma_get_request");
	goto out_destroy_listen_ep;
    }

    memset(&qp_attr, 0, sizeof qp_attr);
    memset(&init_attr, 0, sizeof init_attr);
    ret = ibv_query_qp(id->qp, &qp_attr, IBV_QP_CAP, &init_attr);
    if (ret) {
	perror("ibv_query_qp");
	goto out_destroy_accept_ep;
    }
    if (init_attr.cap.max_inline_data >= 16)
	send_flags = IBV_SEND_INLINE;
    else
	printf("rdma_server: device doesn't support IBV_SEND_INLINE, "
	       "using sge sends\n");

    mr = rdma_reg_msgs(id, recv_msg, 16);
    if (!mr) {
	ret = -1;
	perror("rdma_reg_msgs for recv_msg");
	goto out_destroy_accept_ep;
    }
    if ((send_flags & IBV_SEND_INLINE) == 0) {
	send_mr = rdma_reg_msgs(id, send_msg, 16);
	if (!send_mr) {
	    ret = -1;
	    perror("rdma_reg_msgs for send_msg");
	    goto out_dereg_recv;
	}
    }

    ret = rdma_post_recv(id, NULL, recv_msg, 16, mr);
    if (ret) {
	perror("rdma_post_recv");
	goto out_dereg_send;
    }

    ret = rdma_accept(id, NULL);
    if (ret) {
	perror("rdma_accept");
	goto out_dereg_send;
    }

    while ((ret = rdma_get_recv_comp(id, &wc)) == 0);
    if (ret < 0) {
	perror("rdma_get_recv_comp");
	goto out_disconnect;
    }

    ret = rdma_post_send(id, NULL, send_msg, 16, send_mr, send_flags);
    if (ret) {
	perror("rdma_post_send");
	goto out_disconnect;
    }

    while ((ret = rdma_get_send_comp(id, &wc)) == 0);
    if (ret < 0)
	perror("rdma_get_send_comp");
    else
	ret = 0;

out_disconnect:
    rdma_disconnect(id);
out_dereg_send:
    if ((send_flags & IBV_SEND_INLINE) == 0)
	rdma_dereg_mr(send_mr);
out_dereg_recv:
    rdma_dereg_mr(mr);
out_destroy_accept_ep:
    rdma_destroy_ep(id);
out_destroy_listen_ep:
    rdma_destroy_ep(listen_id);
out_free_addrinfo:
    rdma_freeaddrinfo(res);
    return ret;
}

int main(int argc, char **argv)
{
    char hostname[256];
    char hosts[2][256];

    int server_port;

    if (argc != 2) {
	printf("Server usage: %s <port_greater_than_18000>\n",
	       argv[0]);
	return 1;
    }

    server_port = atoi(argv[1]);
    gethostname(hostname, 256);
    struct hostent *host_entry = gethostbyname(argv[1]);
    char *ipaddr =
	inet_ntoa(*((struct in_addr *) host_entry->h_addr_list[0]));

    // Have to launch the TCP server first
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int ret =
	bind(sock, (struct sockaddr *) &server_addr, sizeof(server_addr));
    if (ret) {
	fprintf(stderr, "%d: failed to bind at port %d\n", __LINE__,
		server_port);
    }

    listen(sock, 1);
    run_server(ipaddr, server_port+5);

    // Accept TCP connection after accepting RDMA connection
    int client_sock = accept(sock, NULL, NULL);

    // Cleanup
    send(client_sock, "done", sizeof("done"), 0);
    close(client_sock);
    close(sock);
    fprintf(stderr, "%d: closed sockets.\n", __LINE__);

    return 0;
}

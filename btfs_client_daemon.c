// SPDX-License-Identifier: GPL-2.0
/*
 * BTFS Client Daemon - Netlink <-> Bluetooth bridge
 * Similar to NFS mount helper daemon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <linux/netlink.h>
#include "btfs_protocol.h"

#define NETLINK_BTFS 31
#define MAX_PENDING 100

/* Netlink message format (must match kernel) */
typedef struct {
	uint32_t op;
	uint32_t seq;
	int32_t res;
	uint32_t len;
	uint8_t data[0];
} __attribute__((packed)) nl_msg_t;

/* Pending request tracking */
typedef struct pending_req {
	uint32_t seq;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int done;
	btfs_response_t rsp;
	char data[BTFS_MAX_DATA];
	struct pending_req *next;
} pending_req_t;

/* Global state */
static int bt_sock = -1;
static int nl_sock = -1;
static pthread_mutex_t bt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static pending_req_t *pending_head = NULL;
static volatile int running = 1;

void log_msg(const char *fmt, ...)
{
	va_list ap;
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char ts[64];
	
	strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S]", tm);
	printf("%s ", ts);
	
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

/*
 * Pending request management
 */
static pending_req_t *pending_add(uint32_t seq)
{
	pending_req_t *req = calloc(1, sizeof(*req));
	if (!req)
		return NULL;
	
	req->seq = seq;
	pthread_mutex_init(&req->lock, NULL);
	pthread_cond_init(&req->cond, NULL);
	
	pthread_mutex_lock(&pend_lock);
	req->next = pending_head;
	pending_head = req;
	pthread_mutex_unlock(&pend_lock);
	
	return req;
}

static pending_req_t *pending_find(uint32_t seq)
{
	pending_req_t *req;
	
	pthread_mutex_lock(&pend_lock);
	for (req = pending_head; req; req = req->next) {
		if (req->seq == seq) {
			pthread_mutex_unlock(&pend_lock);
			return req;
		}
	}
	pthread_mutex_unlock(&pend_lock);
	return NULL;
}

static void pending_remove(uint32_t seq)
{
	pending_req_t *req, *prev = NULL;
	
	pthread_mutex_lock(&pend_lock);
	for (req = pending_head; req; prev = req, req = req->next) {
		if (req->seq == seq) {
			if (prev)
				prev->next = req->next;
			else
				pending_head = req->next;
			
			pthread_mutex_destroy(&req->lock);
			pthread_cond_destroy(&req->cond);
			free(req);
			break;
		}
	}
	pthread_mutex_unlock(&pend_lock);
}

/*
 * Bluetooth communication
 */
static int bt_send_request(uint32_t opcode, uint32_t seq, 
			   const void *data, uint32_t len)
{
	btfs_header_t hdr;
	
	pthread_mutex_lock(&bt_lock);
	
	hdr.opcode = opcode;
	hdr.sequence = seq;
	hdr.client_id = getpid();
	hdr.flags = 0;
	hdr.data_len = len;
	
	if (write(bt_sock, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		pthread_mutex_unlock(&bt_lock);
		return -EIO;
	}
	
	if (data && len > 0) {
		if (write(bt_sock, data, len) != len) {
			pthread_mutex_unlock(&bt_lock);
			return -EIO;
		}
	}
	
	pthread_mutex_unlock(&bt_lock);
	return 0;
}

static int bt_wait_response(uint32_t seq, btfs_response_t *rsp,
			    void *data, size_t datalen)
{
	pending_req_t *req = pending_find(seq);
	struct timespec timeout;
	int ret;
	
	if (!req)
		return -EINVAL;
	
	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += 30;
	
	pthread_mutex_lock(&req->lock);
	while (!req->done && running) {
		ret = pthread_cond_timedwait(&req->cond, &req->lock, &timeout);
		if (ret == ETIMEDOUT) {
			pthread_mutex_unlock(&req->lock);
			return -ETIMEDOUT;
		}
	}
	
	if (!running) {
		pthread_mutex_unlock(&req->lock);
		return -EINTR;
	}
	
	memcpy(rsp, &req->rsp, sizeof(*rsp));
	if (data && datalen && req->rsp.data_len > 0) {
		size_t copy = req->rsp.data_len < datalen ? 
			      req->rsp.data_len : datalen;
		memcpy(data, req->data, copy);
	}
	
	pthread_mutex_unlock(&req->lock);
	return 0;
}

/*
 * Bluetooth receive thread
 */
static void *bt_recv_thread(void *arg)
{
	btfs_response_t rsp;
	pending_req_t *req;
	
	log_msg("BT receive thread started");
	
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(bt_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	
	while (running) {
		ssize_t n = read(bt_sock, &rsp, sizeof(rsp));
		
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			if (running)
				log_msg("BT read error: %s", strerror(errno));
			break;
		}
		
		if (n == 0) {
			log_msg("BT connection closed");
			break;
		}
		
		if (n != sizeof(rsp)) {
			log_msg("BT partial read");
			continue;
		}
		
		req = pending_find(rsp.sequence);
		if (!req) {
			log_msg("Unknown sequence: %u", rsp.sequence);
			continue;
		}
		
		pthread_mutex_lock(&req->lock);
		memcpy(&req->rsp, &rsp, sizeof(rsp));
		
		if (rsp.data_len > 0) {
			size_t toread = rsp.data_len < BTFS_MAX_DATA ?
					rsp.data_len : BTFS_MAX_DATA;
			
			n = read(bt_sock, req->data, toread);
			if (n != toread) {
				log_msg("BT data read error");
				req->rsp.result = -EIO;
			}
		}
		
		req->done = 1;
		pthread_cond_signal(&req->cond);
		pthread_mutex_unlock(&req->lock);
	}
	
	log_msg("BT receive thread stopped");
	return NULL;
}

/*
 * Netlink communication
 */
static int nl_send_response(uint32_t pid, uint32_t seq, int32_t res,
			    const void *data, uint32_t len)
{
	struct sockaddr_nl dest;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	struct msghdr mh;
	struct iovec iov;
	char *buf;
	size_t total;
	int ret;
	
	total = NLMSG_SPACE(sizeof(nl_msg_t) + len);
	buf = malloc(total);
	if (!buf)
		return -ENOMEM;
	
	memset(buf, 0, total);
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_len = total;
	nlh->nlmsg_pid = getpid();
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_type = NLMSG_DONE;
	
	msg = (nl_msg_t *)NLMSG_DATA(nlh);
	msg->op = 0;
	msg->seq = seq;
	msg->res = res;
	msg->len = len;
	
	if (data && len)
		memcpy(msg->data, data, len);
	
	memset(&dest, 0, sizeof(dest));
	dest.nl_family = AF_NETLINK;
	dest.nl_pid = pid;
	
	iov.iov_base = buf;
	iov.iov_len = nlh->nlmsg_len;
	
	memset(&mh, 0, sizeof(mh));
	mh.msg_name = &dest;
	mh.msg_namelen = sizeof(dest);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	
	ret = sendmsg(nl_sock, &mh, 0);
	free(buf);
	
	return ret < 0 ? -errno : 0;
}

static void nl_send_hello(void)
{
	struct sockaddr_nl dest;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	struct msghdr mh;
	struct iovec iov;
	char buf[NLMSG_SPACE(sizeof(nl_msg_t))];
	
	memset(buf, 0, sizeof(buf));
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_len = NLMSG_SPACE(sizeof(nl_msg_t));
	nlh->nlmsg_pid = getpid();
	nlh->nlmsg_type = NLMSG_DONE;
	
	msg = (nl_msg_t *)NLMSG_DATA(nlh);
	msg->op = 0;
	msg->seq = 0;
	msg->res = 0;
	msg->len = 0;
	
	memset(&dest, 0, sizeof(dest));
	dest.nl_family = AF_NETLINK;
	dest.nl_pid = 0; // kernel
	
	iov.iov_base = buf;
	iov.iov_len = nlh->nlmsg_len;
	
	memset(&mh, 0, sizeof(mh));
	mh.msg_name = &dest;
	mh.msg_namelen = sizeof(dest);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	
	if (sendmsg(nl_sock, &mh, 0) > 0)
		log_msg("Registered with kernel module");
}

/*
 * Netlink receive thread
 */
static void *nl_recv_thread(void *arg)
{
	struct sockaddr_nl src;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	struct msghdr mh;
	struct iovec iov;
	char buf[8192];
	pending_req_t *pend;
	btfs_response_t brsp;
	char rdata[BTFS_MAX_DATA];
	uint32_t bt_seq;
	int ret;
	
	log_msg("Netlink thread started");
	
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(nl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	
	nlh = (struct nlmsghdr *)buf;
	
	while (running) {
		memset(&iov, 0, sizeof(iov));
		iov.iov_base = buf;
		iov.iov_len = sizeof(buf);
		
		memset(&mh, 0, sizeof(mh));
		mh.msg_name = &src;
		mh.msg_namelen = sizeof(src);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		
		ssize_t n = recvmsg(nl_sock, &mh, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			if (running)
				perror("netlink recvmsg");
			break;
		}
		
		msg = (nl_msg_t *)NLMSG_DATA(nlh);
		
		log_msg("NL: op=%u seq=%u len=%u", msg->op, msg->seq, msg->len);
		
		/* Forward to Bluetooth server */
		bt_seq = msg->seq;
		pend = pending_add(bt_seq);
		if (!pend) {
			nl_send_response(nlh->nlmsg_pid, msg->seq, -ENOMEM, NULL, 0);
			continue;
		}
		
		ret = bt_send_request(msg->op, bt_seq, msg->data, msg->len);
		if (ret < 0) {
			pending_remove(bt_seq);
			nl_send_response(nlh->nlmsg_pid, msg->seq, ret, NULL, 0);
			continue;
		}
		
		ret = bt_wait_response(bt_seq, &brsp, rdata, sizeof(rdata));
		if (ret < 0) {
			pending_remove(bt_seq);
			nl_send_response(nlh->nlmsg_pid, msg->seq, ret, NULL, 0);
			continue;
		}
		
		nl_send_response(nlh->nlmsg_pid, msg->seq, brsp.result,
				 rdata, brsp.data_len);
		
		pending_remove(bt_seq);
		
		log_msg("NL: response sent seq=%u res=%d len=%u",
			msg->seq, brsp.result, brsp.data_len);
	}
	
	log_msg("Netlink thread stopped");
	return NULL;
}

/*
 * Bluetooth connection
 */
static int bt_connect(const char *mac_addr)
{
	struct sockaddr_rc addr = {0};
	bdaddr_t bdaddr;
	int sock, channel = 1;
	
	/* SDP lookup */
	str2ba(mac_addr, &bdaddr);
	
	uint32_t svc_uuid[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
	uuid_t svc;
	sdp_uuid128_create(&svc, &svc_uuid);
	
	sdp_session_t *session = sdp_connect(BDADDR_ANY, &bdaddr, SDP_RETRY_IF_BUSY);
	if (session) {
		sdp_list_t *search = sdp_list_append(NULL, &svc);
		sdp_list_t *response = NULL;
		uint32_t range = 0x0000ffff;
		sdp_list_t *attrid = sdp_list_append(NULL, &range);
		
		if (sdp_service_search_attr_req(session, search,
						SDP_ATTR_REQ_RANGE,
						attrid, &response) == 0) {
			for (sdp_list_t *r = response; r; r = r->next) {
				sdp_record_t *rec = r->data;
				sdp_list_t *proto;
				
				if (sdp_get_access_protos(rec, &proto) == 0) {
					for (sdp_list_t *p = proto; p; p = p->next) {
						sdp_list_t *pds = p->data;
						for (; pds; pds = pds->next) {
							sdp_data_t *d = pds->data;
							int pr = 0;
							for (; d; d = d->next) {
								if (d->dtd == SDP_UUID16 ||
								    d->dtd == SDP_UUID32 ||
								    d->dtd == SDP_UUID128)
									pr = sdp_uuid_to_proto(&d->val.uuid);
								else if (d->dtd == SDP_UINT8 && pr == RFCOMM_UUID)
									channel = d->val.uint8;
							}
						}
					}
					sdp_list_free(proto, 0);
				}
			}
			sdp_list_free(response, 0);
		}
		
		sdp_list_free(search, 0);
		sdp_list_free(attrid, 0);
		sdp_close(session);
	}
	
	log_msg("Connecting to %s channel %d", mac_addr, channel);
	
	sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sock < 0) {
		perror("socket");
		return -1;
	}
	
	addr.rc_family = AF_BLUETOOTH;
	addr.rc_channel = channel;
	bacpy(&addr.rc_bdaddr, &bdaddr);
	
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(sock);
		return -1;
	}
	
	log_msg("Connected to BTFS server");
	return sock;
}

/*
 * Signal handler
 */
static void sighandler(int sig)
{
	if (sig == SIGINT || sig == SIGTERM) {
		log_msg("Received signal %d", sig);
		running = 0;
	}
}

/*
 * Main
 */
int main(int argc, char **argv)
{
	struct sockaddr_nl local;
	pthread_t bt_thread, nl_thread;
	
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <server_mac>\n", argv[0]);
		return 1;
	}
	
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);
	
	log_msg("==============================================");
	log_msg("  BTFS Client Daemon");
	log_msg("==============================================");
	log_msg("Server: %s", argv[1]);
	
	/* Connect to Bluetooth server */
	bt_sock = bt_connect(argv[1]);
	if (bt_sock < 0) {
		log_msg("Failed to connect to server");
		return 1;
	}
	
	/* Create netlink socket */
	nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_BTFS);
	if (nl_sock < 0) {
		perror("netlink socket");
		log_msg("Make sure kernel module is loaded:");
		log_msg("  sudo insmod btfs_client_fs.ko");
		close(bt_sock);
		return 1;
	}
	
	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	local.nl_pid = getpid();
	
	if (bind(nl_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("netlink bind");
		close(nl_sock);
		close(bt_sock);
		return 1;
	}
	
	log_msg("Netlink socket bound");
	
	/* Send hello to kernel */
	nl_send_hello();
	
	log_msg("==============================================\n");
	
	/* Start threads */
	if (pthread_create(&bt_thread, NULL, bt_recv_thread, NULL) != 0) {
		perror("pthread_create bt");
		close(nl_sock);
		close(bt_sock);
		return 1;
	}
	
	if (pthread_create(&nl_thread, NULL, nl_recv_thread, NULL) != 0) {
		perror("pthread_create nl");
		running = 0;
		pthread_join(bt_thread, NULL);
		close(nl_sock);
		close(bt_sock);
		return 1;
	}
	
	log_msg("Daemon running. Press Ctrl+C to stop.");
	
	/* Wait for threads */
	pthread_join(bt_thread, NULL);
	pthread_join(nl_thread, NULL);
	
	/* Cleanup */
	close(nl_sock);
	close(bt_sock);
	
	pending_req_t *req;
	while ((req = pending_head)) {
		pending_head = req->next;
		pthread_mutex_destroy(&req->lock);
		pthread_cond_destroy(&req->cond);
		free(req);
	}
	
	log_msg("==============================================");
	log_msg("Daemon stopped");
	log_msg("==============================================");
	
	return 0;
}

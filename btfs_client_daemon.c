// SPDX-License-Identifier: GPL-2.0
/*
 * BTFS Client Daemon - Complete version with PING keepalive
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <linux/netlink.h>
#include "btfs_protocol.h"

#define NETLINK_BTFS 31
#define MAX_MSG_SIZE 8192
#define PING_INTERVAL 5

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
	uint32_t nl_seq;  // netlink sequence
	uint32_t bt_seq;  // bluetooth sequence  
	uint32_t nl_pid;  // netlink sender PID
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
static uint32_t bt_seq_gen = 1;

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
static pending_req_t *pending_add(uint32_t nl_seq, uint32_t nl_pid, uint32_t bt_seq)
{
	pending_req_t *req = calloc(1, sizeof(*req));
	if (!req)
		return NULL;
	
	req->nl_seq = nl_seq;
	req->nl_pid = nl_pid;
	req->bt_seq = bt_seq;
	pthread_mutex_init(&req->lock, NULL);
	pthread_cond_init(&req->cond, NULL);
	
	pthread_mutex_lock(&pend_lock);
	req->next = pending_head;
	pending_head = req;
	pthread_mutex_unlock(&pend_lock);
	
	return req;
}

static pending_req_t *pending_find_by_bt(uint32_t bt_seq)
{
	pending_req_t *req;
	
	pthread_mutex_lock(&pend_lock);
	for (req = pending_head; req; req = req->next) {
		if (req->bt_seq == bt_seq) {
			pthread_mutex_unlock(&pend_lock);
			return req;
		}
	}
	pthread_mutex_unlock(&pend_lock);
	return NULL;
}

static void pending_remove(uint32_t bt_seq)
{
	pending_req_t *req, *prev = NULL;
	
	pthread_mutex_lock(&pend_lock);
	for (req = pending_head; req; prev = req, req = req->next) {
		if (req->bt_seq == bt_seq) {
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
		if (write(bt_sock, data, len) != (ssize_t)len) {
			pthread_mutex_unlock(&bt_lock);
			return -EIO;
		}
	}
	
	pthread_mutex_unlock(&bt_lock);
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
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};  // Увеличить timeout до 5 секунд
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
        
        /* Skip PING responses */
        if (rsp.opcode == BTFS_OP_PING)
            continue;
        
        req = pending_find_by_bt(rsp.sequence);
        if (!req) {
            log_msg("Unknown BT sequence: %u", rsp.sequence);
            continue;
        }
        
        pthread_mutex_lock(&req->lock);
        memcpy(&req->rsp, &rsp, sizeof(rsp));
        
        // ИСПРАВЛЕНО: использовать recv() с MSG_WAITALL
        if (rsp.data_len > 0) {
            size_t toread = rsp.data_len < BTFS_MAX_DATA ?
                            rsp.data_len : BTFS_MAX_DATA;
            
            log_msg("BT reading %zu bytes of data", toread);
            
            // MSG_WAITALL - ждать пока не получим ВСЕ данные
            n = recv(bt_sock, req->data, toread, MSG_WAITALL);
            
            if (n != (ssize_t)toread) {
                log_msg("BT data recv failed: got %zd, expected %zu, errno=%s",
                        n, toread, strerror(errno));
                req->rsp.result = -EIO;
            } else {
                log_msg("BT data recv success: %zu bytes", toread);
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
 * PING keepalive thread
 */
static void *ping_thread(void *arg)
{
	btfs_header_t hdr;
	
	log_msg("PING thread started");
	
	memset(&hdr, 0, sizeof(hdr));
	hdr.opcode = BTFS_OP_PING;
	hdr.sequence = 0;
	hdr.client_id = getpid();
	hdr.flags = 0;
	hdr.data_len = 0;
	
	while (running) {
		sleep(PING_INTERVAL);
		
		if (!running)
			break;
		
		pthread_mutex_lock(&bt_lock);
		ssize_t ret = write(bt_sock, &hdr, sizeof(hdr));
		pthread_mutex_unlock(&bt_lock);
		
		if (ret != sizeof(hdr)) {
			log_msg("PING send failed, stopping");
			running = 0;
			break;
		}
	}
	
	log_msg("PING thread stopped");
	return NULL;
}

/*
 * Netlink - send response back to kernel
 */
static int nl_send_reply(uint32_t dst_pid, uint32_t seq, int32_t result,
			 const void *data, uint32_t datalen)
{
	struct sockaddr_nl dest;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	char buf[MAX_MSG_SIZE];
	size_t total;
	
	if (datalen > BTFS_MAX_DATA)
		datalen = BTFS_MAX_DATA;
	
	total = NLMSG_SPACE(sizeof(nl_msg_t) + datalen);
	if (total > sizeof(buf)) {
		log_msg("Message too large: %zu", total);
		return -EMSGSIZE;
	}
	
	memset(buf, 0, total);
	
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(nl_msg_t) + datalen);
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = getpid();
	
	msg = (nl_msg_t *)NLMSG_DATA(nlh);
	msg->op = 0;
	msg->seq = seq;
	msg->res = result;
	msg->len = datalen;
	
	if (data && datalen > 0)
		memcpy(msg->data, data, datalen);
	
	memset(&dest, 0, sizeof(dest));
	dest.nl_family = AF_NETLINK;
	dest.nl_pid = dst_pid;
	dest.nl_groups = 0;
	
	if (sendto(nl_sock, nlh, nlh->nlmsg_len, 0,
		   (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		log_msg("Netlink sendto failed: %s", strerror(errno));
		return -errno;
	}
	
	return 0;
}

/*
 * Netlink receive thread
 */
static void *nl_recv_thread(void *arg)
{
	struct sockaddr_nl src;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	char buf[MAX_MSG_SIZE];
	socklen_t addrlen;
	pending_req_t *pend;
	uint32_t bt_seq;
	int ret;
	struct timespec timeout;
	
	log_msg("Netlink receive thread started");
	
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(nl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	
	while (running) {
		addrlen = sizeof(src);
		memset(buf, 0, sizeof(buf));
		
		ssize_t n = recvfrom(nl_sock, buf, sizeof(buf), 0,
				     (struct sockaddr *)&src, &addrlen);
		
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			if (running)
				log_msg("Netlink recvfrom error: %s", strerror(errno));
			break;
		}
		
		if (n < (ssize_t)sizeof(struct nlmsghdr))
			continue;
		
		nlh = (struct nlmsghdr *)buf;
		
		if (n < (ssize_t)nlh->nlmsg_len)
			continue;
		
		if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(nl_msg_t)))
			continue;
		
		msg = (nl_msg_t *)NLMSG_DATA(nlh);
		
		/* Allocate bluetooth sequence */
		bt_seq = __sync_fetch_and_add(&bt_seq_gen, 1);
		
		/* Create pending request */
		pend = pending_add(msg->seq, nlh->nlmsg_pid, bt_seq);
		if (!pend) {
			nl_send_reply(nlh->nlmsg_pid, msg->seq, -ENOMEM, NULL, 0);
			continue;
		}
		
		/* Forward to Bluetooth server */
		ret = bt_send_request(msg->op, bt_seq, msg->data, msg->len);
		if (ret < 0) {
			log_msg("BT send failed: %d", ret);
			nl_send_reply(pend->nl_pid, pend->nl_seq, ret, NULL, 0);
			pending_remove(bt_seq);
			continue;
		}
		
		/* Wait for Bluetooth response */
		clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_sec += 30;
		
		pthread_mutex_lock(&pend->lock);
		while (!pend->done && running) {
			ret = pthread_cond_timedwait(&pend->cond, &pend->lock, &timeout);
			if (ret == ETIMEDOUT) {
				log_msg("BT timeout for seq=%u", bt_seq);
				pend->rsp.result = -ETIMEDOUT;
				pend->done = 1;
				break;
			}
		}
		
		/* Send response back to kernel */
		nl_send_reply(pend->nl_pid, pend->nl_seq,
			      pend->rsp.result, pend->data, pend->rsp.data_len);
		
		pthread_mutex_unlock(&pend->lock);
		pending_remove(bt_seq);
	}
	
	log_msg("Netlink receive thread stopped");
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
	
	str2ba(mac_addr, &bdaddr);
	
	/* Try SDP lookup */
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
	pthread_t bt_thread, nl_thread, ping_th;
	
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <server_mac>\n", argv[0]);
		return 1;
	}
	
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);
	
	log_msg("==============================================");
	log_msg("  BTFS Client Daemon v2.0");
	log_msg("==============================================");
	
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
	local.nl_groups = 0;
	
	if (bind(nl_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("netlink bind");
		close(nl_sock);
		close(bt_sock);
		return 1;
	}
	
	log_msg("Netlink socket bound (PID=%u)", getpid());
	
	/* Send hello to kernel */
	struct sockaddr_nl dest;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	char hello[NLMSG_SPACE(sizeof(nl_msg_t))];
	
	memset(hello, 0, sizeof(hello));
	nlh = (struct nlmsghdr *)hello;
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(nl_msg_t));
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_seq = 0;
	nlh->nlmsg_pid = getpid();
	
	msg = (nl_msg_t *)NLMSG_DATA(nlh);
	msg->op = 0;
	msg->seq = 0;
	msg->res = 0;
	msg->len = 0;
	
	memset(&dest, 0, sizeof(dest));
	dest.nl_family = AF_NETLINK;
	dest.nl_pid = 0;
	dest.nl_groups = 0;
	
	if (sendto(nl_sock, nlh, nlh->nlmsg_len, 0,
		   (struct sockaddr *)&dest, sizeof(dest)) > 0) {
		log_msg("Sent hello to kernel");
	}
	
	log_msg("PING interval: %d seconds", PING_INTERVAL);
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
	
	if (pthread_create(&ping_th, NULL, ping_thread, NULL) != 0) {
		perror("pthread_create ping");
		running = 0;
		pthread_join(bt_thread, NULL);
		pthread_join(nl_thread, NULL);
		close(nl_sock);
		close(bt_sock);
		return 1;
	}
	
	log_msg("Daemon running. Press Ctrl+C to stop.");
	
	/* Wait */
	pthread_join(bt_thread, NULL);
	pthread_join(nl_thread, NULL);
	pthread_join(ping_th, NULL);
	
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

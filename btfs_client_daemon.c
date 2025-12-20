#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include "btfs_protocol.h"

#define NETLINK_BTFS		31
#define MAX_MSG_SIZE		8192
#define PING_INTERVAL		5
#define RESPONSE_TIMEOUT	30
#define RFCOMM_CHANNEL		1

typedef struct {
	uint32_t op;
	uint32_t seq;
	int32_t res;
	uint32_t len;
	uint8_t data[0];
} __attribute__((packed)) nl_msg_t;

typedef struct pending_req {
	uint32_t nl_seq;
	uint32_t bt_seq;
	uint32_t nl_pid;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int done;
	btfs_response_t rsp;
	char data[BTFS_MAX_DATA];
	struct pending_req *next;
} pending_req_t;

static int bt_sock = -1;
static int nl_sock = -1;
static pthread_mutex_t bt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static pending_req_t *pending_head;
static volatile int running = 1;
static uint32_t bt_seq_gen = 1;

static void log_msg(const char *fmt, ...)
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
	putchar('\n');
	fflush(stdout);
}

/* ========== Pending request management ========== */

static pending_req_t *pending_add(uint32_t nl_seq, uint32_t nl_pid,
				  uint32_t bt_seq)
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

/* ========== Bluetooth communication ========== */

static int bt_send_request(uint32_t opcode, uint32_t seq,
			   const void *data, uint32_t len)
{
	btfs_header_t hdr = {
		.opcode = opcode,
		.sequence = seq,
		.client_id = getpid(),
		.flags = 0,
		.data_len = len,
	};
	size_t sent;
	ssize_t n;

	pthread_mutex_lock(&bt_lock);

	sent = 0;
	while (sent < sizeof(hdr)) {
		n = write(bt_sock, (char *)&hdr + sent, sizeof(hdr) - sent);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			pthread_mutex_unlock(&bt_lock);
			return -EIO;
		}
		sent += n;
	}

	if (data && len > 0) {
		sent = 0;
		while (sent < len) {
			n = write(bt_sock, (char *)data + sent, len - sent);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				pthread_mutex_unlock(&bt_lock);
				return -EIO;
			}
			sent += n;
		}
	}

	pthread_mutex_unlock(&bt_lock);
	return 0;
}

static void *bt_recv_thread(void *arg)
{
	btfs_response_t rsp;
	pending_req_t *req;
	struct timeval tv = { .tv_sec = 5 };
	ssize_t n;

	setsockopt(bt_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (running) {
		n = read(bt_sock, &rsp, sizeof(rsp));
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

		if (n != sizeof(rsp))
			continue;

		if (rsp.opcode == BTFS_OP_PING)
			continue;

		req = pending_find_by_bt(rsp.sequence);
		if (!req) {
			log_msg("Unknown BT sequence: %u", rsp.sequence);
			continue;
		}

		pthread_mutex_lock(&req->lock);
		memcpy(&req->rsp, &rsp, sizeof(rsp));

		if (rsp.data_len > 0) {
			size_t toread = rsp.data_len < BTFS_MAX_DATA ?
					rsp.data_len : BTFS_MAX_DATA;
			n = recv(bt_sock, req->data, toread, MSG_WAITALL);
			if (n != (ssize_t)toread)
				req->rsp.result = -EIO;
		}

		req->done = 1;
		pthread_cond_signal(&req->cond);
		pthread_mutex_unlock(&req->lock);
	}

	return NULL;
}

static void *ping_thread(void *arg)
{
	btfs_header_t hdr = {
		.opcode = BTFS_OP_PING,
		.client_id = getpid(),
	};

	while (running) {
		sleep(PING_INTERVAL);
		if (!running)
			break;

		pthread_mutex_lock(&bt_lock);
		if (write(bt_sock, &hdr, sizeof(hdr)) != sizeof(hdr)) {
			pthread_mutex_unlock(&bt_lock);
			running = 0;
			break;
		}
		pthread_mutex_unlock(&bt_lock);
	}

	return NULL;
}

/* ========== Netlink communication ========== */

static int nl_send_reply(uint32_t dst_pid, uint32_t seq, int32_t result,
			 const void *data, uint32_t datalen)
{
	struct sockaddr_nl dest = { .nl_family = AF_NETLINK, .nl_pid = dst_pid };
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	char buf[MAX_MSG_SIZE];
	size_t total;

	if (datalen > BTFS_MAX_DATA)
		datalen = BTFS_MAX_DATA;

	total = NLMSG_SPACE(sizeof(nl_msg_t) + datalen);
	if (total > sizeof(buf))
		return -EMSGSIZE;

	memset(buf, 0, total);
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(nl_msg_t) + datalen);
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = getpid();

	msg = (nl_msg_t *)NLMSG_DATA(nlh);
	msg->seq = seq;
	msg->res = result;
	msg->len = datalen;

	if (data && datalen > 0)
		memcpy(msg->data, data, datalen);

	if (sendto(nl_sock, nlh, nlh->nlmsg_len, 0,
		   (struct sockaddr *)&dest, sizeof(dest)) < 0)
		return -errno;

	return 0;
}

static void *nl_recv_thread(void *arg)
{
	struct sockaddr_nl src;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	char buf[MAX_MSG_SIZE];
	socklen_t addrlen;
	pending_req_t *pend;
	uint32_t bt_seq;
	struct timespec timeout;
	struct timeval tv = { .tv_sec = 1 };
	int ret;

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
				log_msg("Netlink recv: %s", strerror(errno));
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
		bt_seq = __sync_fetch_and_add(&bt_seq_gen, 1);

		pend = pending_add(msg->seq, nlh->nlmsg_pid, bt_seq);
		if (!pend) {
			nl_send_reply(nlh->nlmsg_pid, msg->seq, -ENOMEM, NULL, 0);
			continue;
		}

		ret = bt_send_request(msg->op, bt_seq, msg->data, msg->len);
		if (ret < 0) {
			nl_send_reply(pend->nl_pid, pend->nl_seq, ret, NULL, 0);
			pending_remove(bt_seq);
			continue;
		}

		clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_sec += RESPONSE_TIMEOUT;

		pthread_mutex_lock(&pend->lock);
		while (!pend->done && running) {
			ret = pthread_cond_timedwait(&pend->cond, &pend->lock,
						     &timeout);
			if (ret == ETIMEDOUT) {
				pend->rsp.result = -ETIMEDOUT;
				pend->done = 1;
				break;
			}
		}

		nl_send_reply(pend->nl_pid, pend->nl_seq,
			      pend->rsp.result, pend->data, pend->rsp.data_len);
		pthread_mutex_unlock(&pend->lock);
		pending_remove(bt_seq);
	}

	return NULL;
}

/* ========== Bluetooth connection ========== */

static int bt_connect(const char *mac_addr)
{
	struct sockaddr_rc addr = {
		.rc_family = AF_BLUETOOTH,
		.rc_channel = RFCOMM_CHANNEL,
	};
	bdaddr_t bdaddr;
	int sock;

	str2ba(mac_addr, &bdaddr);
	bacpy(&addr.rc_bdaddr, &bdaddr);

	log_msg("Connecting to %s channel %d", mac_addr, RFCOMM_CHANNEL);

	sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sock < 0) {
		perror("socket");
		return -1;
	}

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(sock);
		return -1;
	}

	log_msg("Connected to BTFS server");
	return sock;
}

/* ========== Signal handling ========== */

static void sighandler(int sig)
{
	if (sig == SIGINT || sig == SIGTERM)
		running = 0;
}

/* ========== Main ========== */

int main(int argc, char **argv)
{
	struct sockaddr_nl local = { .nl_family = AF_NETLINK, .nl_pid = getpid() };
	struct sockaddr_nl dest = { .nl_family = AF_NETLINK };
	pthread_t bt_thread, nl_thread, ping_th;
	struct nlmsghdr *nlh;
	nl_msg_t *msg;
	char hello[NLMSG_SPACE(sizeof(nl_msg_t))];

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <BT_MAC_ADDRESS>\n", argv[0]);
		return 1;
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	log_msg("==============================================");
	log_msg(" BTFS Client Daemon v2.0");
	log_msg("==============================================");

	bt_sock = bt_connect(argv[1]);
	if (bt_sock < 0) {
		log_msg("Failed to connect to server");
		return 1;
	}

	nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_BTFS);
	if (nl_sock < 0) {
		perror("netlink socket");
		log_msg("Make sure kernel module is loaded:");
		log_msg("  sudo insmod btfs_client_fs.ko");
		close(bt_sock);
		return 1;
	}

	if (bind(nl_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("netlink bind");
		close(nl_sock);
		close(bt_sock);
		return 1;
	}

	log_msg("Netlink socket bound (PID=%u)", getpid());

	memset(hello, 0, sizeof(hello));
	nlh = (struct nlmsghdr *)hello;
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(nl_msg_t));
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_pid = getpid();
	msg = (nl_msg_t *)NLMSG_DATA(nlh);
	msg->seq = 0;

	if (sendto(nl_sock, nlh, nlh->nlmsg_len, 0,
		   (struct sockaddr *)&dest, sizeof(dest)) > 0)
		log_msg("Sent hello to kernel");

	log_msg("RFCOMM channel: %d", RFCOMM_CHANNEL);
	log_msg("PING interval: %d seconds", PING_INTERVAL);
	log_msg("==============================================\n");

	if (pthread_create(&bt_thread, NULL, bt_recv_thread, NULL) != 0 ||
	    pthread_create(&nl_thread, NULL, nl_recv_thread, NULL) != 0 ||
	    pthread_create(&ping_th, NULL, ping_thread, NULL) != 0) {
		perror("pthread_create");
		running = 0;
		goto cleanup;
	}

	log_msg("Daemon running. Press Ctrl+C to stop.");

	pthread_join(bt_thread, NULL);
	pthread_join(nl_thread, NULL);
	pthread_join(ping_th, NULL);

cleanup:
	close(nl_sock);
	close(bt_sock);

	while (pending_head) {
		pending_req_t *req = pending_head;
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

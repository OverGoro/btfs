// btfs_client_daemon.c - Daemon для связи BT ↔ Netlink
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/netlink.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <stdarg.h>
#include <time.h>

#include "btfs_protocol.h"

#define NETLINK_BTFS 31
#define MAX_PENDING_REQUESTS 100

// Структура ожидающего запроса
typedef struct pending_request
{
    uint32_t sequence;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int completed;
    btfs_response_t response;
    char response_data[BTFS_MAX_DATA];
    struct pending_request *next;
} pending_request_t;

// Глобальные переменные
static int bt_socket = -1;
static int nl_socket = -1;
static pthread_mutex_t bt_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static pending_request_t *pending_requests = NULL;
static uint32_t sequence_counter = 1;
static volatile int running = 1;
static uint32_t client_id = 0;

void print_log(const char *format, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", t);

    printf("%s ", timestamp);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

// ============ PENDING REQUESTS ============

pending_request_t *create_pending_request(uint32_t sequence)
{
    pending_request_t *req = malloc(sizeof(pending_request_t));
    if (!req)
        return NULL;

    req->sequence = sequence;
    req->completed = 0;
    pthread_mutex_init(&req->mutex, NULL);
    pthread_cond_init(&req->cond, NULL);

    pthread_mutex_lock(&pending_mutex);
    req->next = pending_requests;
    pending_requests = req;
    pthread_mutex_unlock(&pending_mutex);

    return req;
}

pending_request_t *find_pending_request(uint32_t sequence)
{
    pthread_mutex_lock(&pending_mutex);

    pending_request_t *req = pending_requests;
    while (req)
    {
        if (req->sequence == sequence)
        {
            pthread_mutex_unlock(&pending_mutex);
            return req;
        }
        req = req->next;
    }

    pthread_mutex_unlock(&pending_mutex);
    return NULL;
}

void remove_pending_request(uint32_t sequence)
{
    pthread_mutex_lock(&pending_mutex);

    pending_request_t *req = pending_requests;
    pending_request_t *prev = NULL;

    while (req)
    {
        if (req->sequence == sequence)
        {
            if (prev)
            {
                prev->next = req->next;
            }
            else
            {
                pending_requests = req->next;
            }

            pthread_mutex_destroy(&req->mutex);
            pthread_cond_destroy(&req->cond);
            free(req);
            break;
        }
        prev = req;
        req = req->next;
    }

    pthread_mutex_unlock(&pending_mutex);
}

// ============ BLUETOOTH COMMUNICATION ============

int send_bt_request(uint32_t opcode, uint32_t flags, const void *data,
                    uint32_t data_len, uint32_t *sequence_out)
{
    btfs_header_t header;

    pthread_mutex_lock(&bt_mutex);

    header.opcode = opcode;
    header.sequence = sequence_counter++;
    header.client_id = client_id;
    header.flags = flags;
    header.data_len = data_len;

    *sequence_out = header.sequence;

    if (write(bt_socket, &header, sizeof(header)) != sizeof(header))
    {
        pthread_mutex_unlock(&bt_mutex);
        return -EIO;
    }

    if (data && data_len > 0)
    {
        if (write(bt_socket, data, data_len) != data_len)
        {
            pthread_mutex_unlock(&bt_mutex);
            return -EIO;
        }
    }

    pthread_mutex_unlock(&bt_mutex);
    return 0;
}

int wait_for_response(uint32_t sequence, btfs_response_t *response,
                      void *data_buffer, size_t buffer_size)
{
    pending_request_t *req = find_pending_request(sequence);
    if (!req)
    {
        return -EINVAL;
    }

    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30;

    pthread_mutex_lock(&req->mutex);

    while (!req->completed && running)
    {
        int ret = pthread_cond_timedwait(&req->cond, &req->mutex, &timeout);
        if (ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&req->mutex);
            remove_pending_request(sequence);
            return -ETIMEDOUT;
        }
    }

    if (!running)
    {
        pthread_mutex_unlock(&req->mutex);
        remove_pending_request(sequence);
        return -EINTR;
    }

    memcpy(response, &req->response, sizeof(btfs_response_t));

    if (data_buffer && buffer_size > 0 && req->response.data_len > 0)
    {
        size_t copy_len = (req->response.data_len < buffer_size) ? req->response.data_len : buffer_size;
        memcpy(data_buffer, req->response_data, copy_len);
    }

    pthread_mutex_unlock(&req->mutex);
    remove_pending_request(sequence);

    return 0;
}

void *bt_receive_thread(void *arg)
{
    btfs_response_t response;
    char data_buffer[BTFS_MAX_DATA];

    print_log("BT receive thread started");

    while (running)
    {
        ssize_t ret = read(bt_socket, &response, sizeof(response));
        if (ret != sizeof(response))
        {
            if (running)
            {
                print_log("ERROR: Failed to read response header");
            }
            break;
        }

        if (response.data_len > 0)
        {
            if (response.data_len > sizeof(data_buffer))
            {
                print_log("ERROR: Response data too large: %u", response.data_len);
                break;
            }

            ret = read(bt_socket, data_buffer, response.data_len);
            if (ret != response.data_len)
            {
                print_log("ERROR: Failed to read response data");
                break;
            }
        }

        pending_request_t *req = find_pending_request(response.sequence);
        if (req)
        {
            pthread_mutex_lock(&req->mutex);

            memcpy(&req->response, &response, sizeof(response));
            if (response.data_len > 0)
            {
                memcpy(req->response_data, data_buffer, response.data_len);
            }

            req->completed = 1;
            pthread_cond_signal(&req->cond);

            pthread_mutex_unlock(&req->mutex);
        }
        else
        {
            print_log("WARNING: Response for unknown sequence: %u",
                      response.sequence);
        }
    }

    print_log("BT receive thread stopped");
    return NULL;
}

// ============ NETLINK COMMUNICATION ============

typedef struct
{
    uint32_t opcode;
    uint32_t sequence;
    int32_t result;
    uint32_t data_len;
    char data[0];
} nl_message_t;

int send_netlink_response(int pid, uint32_t sequence, int32_t result,
                          const void *data, uint32_t data_len)
{
    struct sockaddr_nl dest_addr;
    struct nlmsghdr *nlh;
    struct msghdr msg;
    struct iovec iov;
    nl_message_t *nl_msg;
    char *buf;

    size_t total_len = NLMSG_SPACE(sizeof(nl_message_t) + data_len);
    buf = malloc(total_len);
    if (!buf)
        return -ENOMEM;

    memset(buf, 0, total_len);

    nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len = total_len;
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;

    nl_msg = (nl_message_t *)NLMSG_DATA(nlh);
    nl_msg->opcode = 0;
    nl_msg->sequence = sequence;
    nl_msg->result = result;
    nl_msg->data_len = data_len;

    if (data && data_len > 0)
    {
        memcpy(nl_msg->data, data, data_len);
    }

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = pid;

    iov.iov_base = buf;
    iov.iov_len = nlh->nlmsg_len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &dest_addr;
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    int ret = sendmsg(nl_socket, &msg, 0);
    free(buf);

    return (ret < 0) ? -errno : 0;
}

void *netlink_thread(void *arg)
{
    struct sockaddr_nl src_addr;
    struct nlmsghdr *nlh = NULL;
    struct msghdr msg;
    struct iovec iov;
    char buffer[8192];

    print_log("Netlink thread started");

    nlh = (struct nlmsghdr *)buffer;

    while (running)
    {
        memset(&iov, 0, sizeof(iov));
        iov.iov_base = buffer;
        iov.iov_len = sizeof(buffer);

        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &src_addr;
        msg.msg_namelen = sizeof(src_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        ssize_t ret = recvmsg(nl_socket, &msg, 0);
        if (ret < 0)
        {
            if (running)
            {
                perror("Netlink recvmsg");
            }
            break;
        }

        nl_message_t *nl_msg = (nl_message_t *)NLMSG_DATA(nlh);
        int kernel_pid = nlh->nlmsg_pid;

        print_log("NL: Received opcode=%u, seq=%u, data_len=%u",
                  nl_msg->opcode, nl_msg->sequence, nl_msg->data_len);

        uint32_t bt_sequence;

        int send_ret = send_bt_request(nl_msg->opcode, 0, nl_msg->data,
                                       nl_msg->data_len, &bt_sequence);
        if (send_ret < 0)
        {
            print_log("ERROR: Failed to send BT request: %d", send_ret);
            send_netlink_response(kernel_pid, nl_msg->sequence, send_ret, NULL, 0);
            continue;
        }

        pending_request_t *pending = create_pending_request(bt_sequence);
        if (!pending)
        {
            print_log("ERROR: Failed to create pending request");
            send_netlink_response(kernel_pid, nl_msg->sequence, -ENOMEM, NULL, 0);
            continue;
        }

        btfs_response_t bt_response;
        char response_data[BTFS_MAX_DATA];

        int wait_ret = wait_for_response(bt_sequence, &bt_response,
                                         response_data, sizeof(response_data));
        if (wait_ret < 0)
        {
            print_log("ERROR: Failed to get BT response: %d", wait_ret);
            send_netlink_response(kernel_pid, nl_msg->sequence, wait_ret, NULL, 0);
            continue;
        }

        send_netlink_response(kernel_pid, nl_msg->sequence, bt_response.result,
                              response_data, bt_response.data_len);

        print_log("NL: Sent response seq=%u, result=%d, data_len=%u",
                  nl_msg->sequence, bt_response.result, bt_response.data_len);
    }

    print_log("Netlink thread stopped");
    return NULL;
}

// ============ SERVER CONNECTION ============

int find_server_and_connect(const char *server_mac, const char *server_name)
{
    bdaddr_t server_addr;
    uint8_t channel = 1;

    if (server_mac)
    {
        str2ba(server_mac, &server_addr);
    }
    else
    {
        print_log("ERROR: Server MAC required");
        return -1;
    }

    // SDP query
    uint32_t service_uuid_int[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
    uuid_t svc_uuid;
    sdp_uuid128_create(&svc_uuid, &service_uuid_int);

    sdp_session_t *session = sdp_connect(BDADDR_ANY, &server_addr, SDP_RETRY_IF_BUSY);
    if (session)
    {
        print_log("Connected to SDP server");

        sdp_list_t *search_list = sdp_list_append(NULL, &svc_uuid);
        sdp_list_t *response_list = NULL;
        uint32_t range = 0x0000ffff;
        sdp_list_t *attrid_list = sdp_list_append(NULL, &range);

        if (sdp_service_search_attr_req(session, search_list,
                                        SDP_ATTR_REQ_RANGE, attrid_list,
                                        &response_list) == 0 &&
            response_list)
        {
            sdp_list_t *r = response_list;
            for (; r; r = r->next)
            {
                sdp_record_t *rec = (sdp_record_t *)r->data;
                sdp_list_t *proto_list;

                if (sdp_get_access_protos(rec, &proto_list) == 0)
                {
                    sdp_list_t *p = proto_list;
                    for (; p; p = p->next)
                    {
                        sdp_list_t *pds = (sdp_list_t *)p->data;
                        for (; pds; pds = pds->next)
                        {
                            sdp_data_t *d = (sdp_data_t *)pds->data;
                            int proto = 0;
                            for (; d; d = d->next)
                            {
                                switch (d->dtd)
                                {
                                case SDP_UUID16:
                                case SDP_UUID32:
                                case SDP_UUID128:
                                    proto = sdp_uuid_to_proto(&d->val.uuid);
                                    break;
                                case SDP_UINT8:
                                    if (proto == RFCOMM_UUID)
                                    {
                                        channel = d->val.uint8;
                                        print_log("Found BTFS on channel %d", channel);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    sdp_list_free(proto_list, 0);
                }
                sdp_record_free(rec);
            }
            sdp_list_free(response_list, 0);
        }

        sdp_list_free(search_list, 0);
        sdp_list_free(attrid_list, 0);
        sdp_close(session);
    }

    print_log("Connecting to server on channel %d...", channel);

    int sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock < 0)
    {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_rc addr = {0};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = channel;
    bacpy(&addr.rc_bdaddr, &server_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Connection failed");
        close(sock);
        return -1;
    }

    print_log("Connected to BTFS server successfully");

    client_id = getpid();

    return sock;
}

// ============ MAIN ============

void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        print_log("Received shutdown signal");
        running = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <server_mac_address>\n", argv[0]);
        return 1;
    }

    const char *server_mac = argv[1];

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    print_log("===================================================");
    print_log("  BTFS CLIENT DAEMON");
    print_log("===================================================");
    print_log("Server MAC: %s", server_mac);

    // Connect to BT server
    bt_socket = find_server_and_connect(server_mac, NULL);
    if (bt_socket < 0)
    {
        print_log("ERROR: Failed to connect to server");
        return 1;
    }

    // Create Netlink socket
    nl_socket = socket(AF_NETLINK, SOCK_RAW, NETLINK_BTFS);
    if (nl_socket < 0)
    {
        perror("Netlink socket creation failed");
        print_log("Make sure kernel module is loaded: sudo insmod btfs_client_fs.ko");
        close(bt_socket);
        return 1;
    }

    struct sockaddr_nl src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();

    if (bind(nl_socket, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0)
    {
        perror("Netlink bind failed");
        close(nl_socket);
        close(bt_socket);
        return 1;
    }

    print_log("Netlink socket created and bound");
    print_log("===================================================\n");

    // Start threads
    pthread_t bt_thread, nl_thread;

    if (pthread_create(&bt_thread, NULL, bt_receive_thread, NULL) != 0)
    {
        perror("Failed to create BT thread");
        close(nl_socket);
        close(bt_socket);
        return 1;
    }

    if (pthread_create(&nl_thread, NULL, netlink_thread, NULL) != 0)
    {
        perror("Failed to create Netlink thread");
        running = 0;
        pthread_join(bt_thread, NULL);
        close(nl_socket);
        close(bt_socket);
        return 1;
    }

    print_log("Client daemon running. Press Ctrl+C to stop.");

    // Keep-alive
    while (running)
    {
        sleep(10);

        uint32_t seq;
        if (send_bt_request(BTFS_OP_PING, 0, NULL, 0, &seq) == 0)
        {
            pending_request_t *pending = create_pending_request(seq);
            if (pending)
            {
                btfs_response_t resp;
                if (wait_for_response(seq, &resp, NULL, 0) < 0)
                {
                    print_log("WARNING: Ping timeout");
                }
            }
        }
    }

    // Cleanup
    print_log("\n===================================================");
    print_log("Shutting down daemon...");

    pthread_join(bt_thread, NULL);
    pthread_join(nl_thread, NULL);

    close(nl_socket);
    close(bt_socket);

    pthread_mutex_lock(&pending_mutex);
    pending_request_t *req = pending_requests;
    while (req)
    {
        pending_request_t *next = req->next;
        pthread_mutex_destroy(&req->mutex);
        pthread_cond_destroy(&req->cond);
        free(req);
        req = next;
    }
    pthread_mutex_unlock(&pending_mutex);

    print_log("Daemon stopped");
    print_log("===================================================");

    return 0;
}

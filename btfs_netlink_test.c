// btfs_netlink_test.c - тестовый клиент для проверки daemon через Netlink
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/netlink.h>
#include <time.h>
#include <stdarg.h>

#include "btfs_protocol.h"

#define NETLINK_BTFS 31

static int nl_socket = -1;
static uint32_t sequence_counter = 1;

// Структура для netlink сообщений
typedef struct {
    uint32_t opcode;
    uint32_t sequence;
    int32_t result;
    uint32_t data_len;
    char data[0];
} nl_message_t;

void print_log(const char *format, ...) {
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

// ============ NETLINK КОММУНИКАЦИЯ ============

int send_nl_request(uint32_t opcode, const void *data, uint32_t data_len) {
    struct sockaddr_nl dest_addr;
    struct nlmsghdr *nlh;
    struct msghdr msg;
    struct iovec iov;
    nl_message_t *nl_msg;
    char *buf;
    
    size_t total_len = NLMSG_SPACE(sizeof(nl_message_t) + data_len);
    buf = malloc(total_len);
    if (!buf) {
        perror("malloc failed");
        return -1;
    }
    
    memset(buf, 0, total_len);
    
    nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len = total_len;
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;
    
    nl_msg = (nl_message_t *)NLMSG_DATA(nlh);
    nl_msg->opcode = opcode;
    nl_msg->sequence = sequence_counter++;
    nl_msg->result = 0;
    nl_msg->data_len = data_len;
    
    if (data && data_len > 0) {
        memcpy(nl_msg->data, data, data_len);
    }
    
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = 0;  // Kernel
    
    iov.iov_base = buf;
    iov.iov_len = nlh->nlmsg_len;
    
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &dest_addr;
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    int ret = sendmsg(nl_socket, &msg, 0);
    free(buf);
    
    if (ret < 0) {
        perror("sendmsg failed");
        return -1;
    }
    
    return 0;
}

int receive_nl_response(void *data_buffer, size_t buffer_size, int32_t *result_out) {
    struct sockaddr_nl src_addr;
    struct nlmsghdr *nlh;
    struct msghdr msg;
    struct iovec iov;
    char buffer[8192];
    
    nlh = (struct nlmsghdr *)buffer;
    
    memset(&iov, 0, sizeof(iov));
    iov.iov_base = buffer;
    iov.iov_len = sizeof(buffer);
    
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &src_addr;
    msg.msg_namelen = sizeof(src_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    ssize_t ret = recvmsg(nl_socket, &msg, 0);
    if (ret < 0) {
        perror("recvmsg failed");
        return -1;
    }
    
    nl_message_t *nl_msg = (nl_message_t *)NLMSG_DATA(nlh);
    
    *result_out = nl_msg->result;
    
    if (data_buffer && buffer_size > 0 && nl_msg->data_len > 0) {
        size_t copy_len = (nl_msg->data_len < buffer_size) ? 
                          nl_msg->data_len : buffer_size;
        memcpy(data_buffer, nl_msg->data, copy_len);
    }
    
    return nl_msg->data_len;
}

// ============ ТЕСТЫ ============

void test_ping() {
    print_log("=== TEST: PING ===");
    
    if (send_nl_request(BTFS_OP_PING, NULL, 0) < 0) {
        print_log("FAILED: send_nl_request");
        return;
    }
    
    int32_t result;
    if (receive_nl_response(NULL, 0, &result) < 0) {
        print_log("FAILED: receive_nl_response");
        return;
    }
    
    if (result == 0) {
        print_log("PASS: Ping successful");
    } else {
        print_log("FAILED: Ping returned error %d", result);
    }
}

void test_getattr(const char *path) {
    print_log("=== TEST: GETATTR %s ===", path);
    
    btfs_getattr_req_t req;
    strncpy(req.path, path, BTFS_MAX_PATH - 1);
    req.path[BTFS_MAX_PATH - 1] = '\0';
    
    if (send_nl_request(BTFS_OP_GETATTR, &req, sizeof(req)) < 0) {
        print_log("FAILED: send_nl_request");
        return;
    }
    
    btfs_attr_t attr;
    int32_t result;
    if (receive_nl_response(&attr, sizeof(attr), &result) < 0) {
        print_log("FAILED: receive_nl_response");
        return;
    }
    
    if (result == 0) {
        print_log("PASS: size=%lu, mode=%o, uid=%u, gid=%u", 
                  attr.size, attr.mode, attr.uid, attr.gid);
    } else {
        print_log("FAILED: Error %d", result);
    }
}

void test_readdir(const char *path) {
    print_log("=== TEST: READDIR %s ===", path);
    
    btfs_readdir_req_t req;
    strncpy(req.path, path, BTFS_MAX_PATH - 1);
    req.path[BTFS_MAX_PATH - 1] = '\0';
    req.offset = 0;
    
    if (send_nl_request(BTFS_OP_READDIR, &req, sizeof(req)) < 0) {
        print_log("FAILED: send_nl_request");
        return;
    }
    
    char buffer[BTFS_MAX_DATA];
    int32_t result;
    int data_len = receive_nl_response(buffer, sizeof(buffer), &result);
    
    if (data_len < 0) {
        print_log("FAILED: receive_nl_response");
        return;
    }
    
    if (result == 0) {
        print_log("PASS: Listed entries:");
        size_t offset = 0;
        while (offset < data_len) {
            btfs_dirent_t *entry = (btfs_dirent_t*)(buffer + offset);
            printf("  - %s (type=%u, ino=%lu)\n", entry->name, entry->type, entry->ino);
            offset += sizeof(btfs_dirent_t) + entry->name_len + 1;
        }
    } else {
        print_log("FAILED: Error %d", result);
    }
}

void test_create_read_write_delete() {
    print_log("=== TEST: CREATE/OPEN/WRITE/READ/CLOSE/UNLINK ===");
    
    const char *test_file = "test_file_nl.txt";
    const char *test_data = "Hello from Netlink test!";
    int32_t result;
    
    // CREATE
    btfs_create_req_t create_req;
    strncpy(create_req.path, test_file, BTFS_MAX_PATH - 1);
    create_req.path[BTFS_MAX_PATH - 1] = '\0';
    create_req.mode = 0644;
    create_req.flags = 0;
    
    if (send_nl_request(BTFS_OP_CREATE, &create_req, sizeof(create_req)) < 0) {
        print_log("FAILED: CREATE send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: CREATE error %d", result);
        return;
    }
    print_log("PASS: File created");
    
    // OPEN
    btfs_open_req_t open_req;
    strncpy(open_req.path, test_file, BTFS_MAX_PATH - 1);
    open_req.path[BTFS_MAX_PATH - 1] = '\0';
    open_req.flags = O_RDWR;
    open_req.mode = 0;
    open_req.lock_type = BTFS_LOCK_NONE;
    
    if (send_nl_request(BTFS_OP_OPEN, &open_req, sizeof(open_req)) < 0) {
        print_log("FAILED: OPEN send");
        return;
    }
    
    btfs_open_resp_t open_resp;
    if (receive_nl_response(&open_resp, sizeof(open_resp), &result) < 0 || result != 0) {
        print_log("FAILED: OPEN error %d", result);
        return;
    }
    uint64_t file_handle = open_resp.file_handle;
    print_log("PASS: File opened, handle=%lu", file_handle);
    
    // WRITE
    size_t write_size = sizeof(btfs_write_req_t) + strlen(test_data);
    btfs_write_req_t *write_req = malloc(write_size);
    write_req->file_handle = file_handle;
    write_req->offset = 0;
    write_req->size = strlen(test_data);
    memcpy(write_req->data, test_data, strlen(test_data));
    
    if (send_nl_request(BTFS_OP_WRITE, write_req, write_size) < 0) {
        print_log("FAILED: WRITE send");
        free(write_req);
        return;
    }
    free(write_req);
    
    btfs_write_resp_t write_resp;
    if (receive_nl_response(&write_resp, sizeof(write_resp), &result) < 0 || result != 0) {
        print_log("FAILED: WRITE error %d", result);
        return;
    }
    print_log("PASS: Written %u bytes", write_resp.bytes_written);
    
    // READ
    btfs_read_req_t read_req;
    read_req.file_handle = file_handle;
    read_req.offset = 0;
    read_req.size = strlen(test_data);
    
    if (send_nl_request(BTFS_OP_READ, &read_req, sizeof(read_req)) < 0) {
        print_log("FAILED: READ send");
        return;
    }
    
    char read_buffer[BTFS_MAX_DATA];
    int data_len = receive_nl_response(read_buffer, sizeof(read_buffer), &result);
    
    if (data_len < 0 || result != 0) {
        print_log("FAILED: READ error %d", result);
        return;
    }
    
    btfs_read_resp_t *read_resp = (btfs_read_resp_t*)read_buffer;
    read_resp->data[read_resp->bytes_read] = '\0';
    
    if (strcmp(read_resp->data, test_data) == 0) {
        print_log("PASS: Read data matches: \"%s\"", read_resp->data);
    } else {
        print_log("FAILED: Read data mismatch: got \"%s\"", read_resp->data);
    }
    
    // CLOSE
    btfs_close_req_t close_req;
    close_req.file_handle = file_handle;
    
    if (send_nl_request(BTFS_OP_CLOSE, &close_req, sizeof(close_req)) < 0) {
        print_log("FAILED: CLOSE send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: CLOSE error %d", result);
        return;
    }
    print_log("PASS: File closed");
    
    // UNLINK
    btfs_unlink_req_t unlink_req;
    strncpy(unlink_req.path, test_file, BTFS_MAX_PATH - 1);
    unlink_req.path[BTFS_MAX_PATH - 1] = '\0';
    
    if (send_nl_request(BTFS_OP_UNLINK, &unlink_req, sizeof(unlink_req)) < 0) {
        print_log("FAILED: UNLINK send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: UNLINK error %d", result);
        return;
    }
    print_log("PASS: File deleted");
}

void test_mkdir_rmdir() {
    print_log("=== TEST: MKDIR/RMDIR ===");
    
    const char *test_dir = "test_directory_nl";
    int32_t result;
    
    // MKDIR
    btfs_mkdir_req_t mkdir_req;
    strncpy(mkdir_req.path, test_dir, BTFS_MAX_PATH - 1);
    mkdir_req.path[BTFS_MAX_PATH - 1] = '\0';
    mkdir_req.mode = 0755;
    
    if (send_nl_request(BTFS_OP_MKDIR, &mkdir_req, sizeof(mkdir_req)) < 0) {
        print_log("FAILED: MKDIR send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: MKDIR error %d", result);
        return;
    }
    print_log("PASS: Directory created");
    
    // RMDIR
    btfs_unlink_req_t rmdir_req;
    strncpy(rmdir_req.path, test_dir, BTFS_MAX_PATH - 1);
    rmdir_req.path[BTFS_MAX_PATH - 1] = '\0';
    
    if (send_nl_request(BTFS_OP_RMDIR, &rmdir_req, sizeof(rmdir_req)) < 0) {
        print_log("FAILED: RMDIR send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: RMDIR error %d", result);
        return;
    }
    print_log("PASS: Directory deleted");
}

void test_locks() {
    print_log("=== TEST: FILE LOCKING ===");
    
    const char *test_file = "lock_test_nl.txt";
    int32_t result;
    
    // CREATE файл
    btfs_create_req_t create_req;
    strncpy(create_req.path, test_file, BTFS_MAX_PATH - 1);
    create_req.path[BTFS_MAX_PATH - 1] = '\0';
    create_req.mode = 0644;
    create_req.flags = 0;
    
    send_nl_request(BTFS_OP_CREATE, &create_req, sizeof(create_req));
    receive_nl_response(NULL, 0, &result);
    
    // LOCK (READ)
    btfs_lock_req_t lock_req;
    strncpy(lock_req.path, test_file, BTFS_MAX_PATH - 1);
    lock_req.path[BTFS_MAX_PATH - 1] = '\0';
    lock_req.lock_type = BTFS_LOCK_READ;
    lock_req.timeout_sec = 0;
    
    if (send_nl_request(BTFS_OP_LOCK, &lock_req, sizeof(lock_req)) < 0) {
        print_log("FAILED: LOCK send");
        return;
    }
    
    btfs_lock_resp_t lock_resp;
    if (receive_nl_response(&lock_resp, sizeof(lock_resp), &result) < 0 || result != 0) {
        print_log("FAILED: LOCK error %d", result);
        return;
    }
    print_log("PASS: Lock acquired, lock_id=%u", lock_resp.lock_id);
    
    // UNLOCK
    if (send_nl_request(BTFS_OP_UNLOCK, &lock_req, sizeof(lock_req)) < 0) {
        print_log("FAILED: UNLOCK send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: UNLOCK error %d", result);
        return;
    }
    print_log("PASS: Lock released");
    
    // Cleanup
    btfs_unlink_req_t unlink_req;
    strncpy(unlink_req.path, test_file, BTFS_MAX_PATH - 1);
    unlink_req.path[BTFS_MAX_PATH - 1] = '\0';
    send_nl_request(BTFS_OP_UNLINK, &unlink_req, sizeof(unlink_req));
    receive_nl_response(NULL, 0, &result);
}

void test_rename() {
    print_log("=== TEST: RENAME ===");
    
    const char *old_name = "rename_test_old.txt";
    const char *new_name = "rename_test_new.txt";
    int32_t result;
    
    // CREATE файл
    btfs_create_req_t create_req;
    strncpy(create_req.path, old_name, BTFS_MAX_PATH - 1);
    create_req.path[BTFS_MAX_PATH - 1] = '\0';
    create_req.mode = 0644;
    create_req.flags = 0;
    
    if (send_nl_request(BTFS_OP_CREATE, &create_req, sizeof(create_req)) < 0) {
        print_log("FAILED: CREATE send");
        return;
    }
    receive_nl_response(NULL, 0, &result);
    
    // RENAME
    btfs_rename_req_t rename_req;
    strncpy(rename_req.oldpath, old_name, BTFS_MAX_PATH - 1);
    rename_req.oldpath[BTFS_MAX_PATH - 1] = '\0';
    strncpy(rename_req.newpath, new_name, BTFS_MAX_PATH - 1);
    rename_req.newpath[BTFS_MAX_PATH - 1] = '\0';
    
    if (send_nl_request(BTFS_OP_RENAME, &rename_req, sizeof(rename_req)) < 0) {
        print_log("FAILED: RENAME send");
        return;
    }
    
    if (receive_nl_response(NULL, 0, &result) < 0 || result != 0) {
        print_log("FAILED: RENAME error %d", result);
        return;
    }
    print_log("PASS: File renamed from %s to %s", old_name, new_name);
    
    // Cleanup
    btfs_unlink_req_t unlink_req;
    strncpy(unlink_req.path, new_name, BTFS_MAX_PATH - 1);
    unlink_req.path[BTFS_MAX_PATH - 1] = '\0';
    send_nl_request(BTFS_OP_UNLINK, &unlink_req, sizeof(unlink_req));
    receive_nl_response(NULL, 0, &result);
}

// ============ MAIN ============

int main(int argc, char **argv) {
    const char *test_name = (argc > 1) ? argv[1] : "all";
    
    print_log("===================================================");
    print_log("  BTFS NETLINK TEST CLIENT");
    print_log("===================================================");
    print_log("Test: %s", test_name);
    print_log("===================================================\n");
    
    // Создание Netlink сокета
    nl_socket = socket(AF_NETLINK, SOCK_RAW, NETLINK_BTFS);
    if (nl_socket < 0) {
        perror("Netlink socket creation failed");
        print_log("ERROR: Make sure btfs_client_daemon is running!");
        return 1;
    }
    
    struct sockaddr_nl src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    
    if (bind(nl_socket, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        perror("Netlink bind failed");
        close(nl_socket);
        return 1;
    }
    
    print_log("Netlink socket connected\n");
    
    // Установить таймаут на чтение
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(nl_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Запуск тестов
    if (strcmp(test_name, "ping") == 0 || strcmp(test_name, "all") == 0) {
        test_ping();
        printf("\n");
    }
    
    if (strcmp(test_name, "getattr") == 0 || strcmp(test_name, "all") == 0) {
        test_getattr(".");
        printf("\n");
    }
    
    if (strcmp(test_name, "readdir") == 0 || strcmp(test_name, "all") == 0) {
        test_readdir(".");
        printf("\n");
    }
    
    if (strcmp(test_name, "file_ops") == 0 || strcmp(test_name, "all") == 0) {
        test_create_read_write_delete();
        printf("\n");
    }
    
    if (strcmp(test_name, "dir_ops") == 0 || strcmp(test_name, "all") == 0) {
        test_mkdir_rmdir();
        printf("\n");
    }
    
    if (strcmp(test_name, "locks") == 0 || strcmp(test_name, "all") == 0) {
        test_locks();
        printf("\n");
    }
    
    if (strcmp(test_name, "rename") == 0 || strcmp(test_name, "all") == 0) {
        test_rename();
        printf("\n");
    }
    
    close(nl_socket);
    
    print_log("===================================================");
    print_log("Tests completed");
    print_log("===================================================");
    
    return 0;
}

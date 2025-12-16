// btfs_test_client.c - тестовый клиент для проверки сервера
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "btfs_protocol.h"

static int bt_socket = -1;
static uint32_t sequence_counter = 1;
static uint32_t client_id = 0;

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

// ============ ПРОТОКОЛ ============

int send_request(uint32_t opcode, const void *data, uint32_t data_len) {
    btfs_header_t header;
    header.opcode = opcode;
    header.sequence = sequence_counter++;
    header.client_id = client_id;
    header.flags = 0;
    header.data_len = data_len;
    
    if (write(bt_socket, &header, sizeof(header)) != sizeof(header)) {
        perror("Failed to send header");
        return -1;
    }
    
    if (data && data_len > 0) {
        if (write(bt_socket, data, data_len) != data_len) {
            perror("Failed to send data");
            return -1;
        }
    }
    
    return 0;
}

int receive_response(btfs_response_t *response, void *data_buffer, size_t buffer_size) {
    if (read(bt_socket, response, sizeof(btfs_response_t)) != sizeof(btfs_response_t)) {
        perror("Failed to read response");
        return -1;
    }
    
    if (response->data_len > 0) {
        if (response->data_len > buffer_size) {
            fprintf(stderr, "Response data too large: %u\n", response->data_len);
            return -1;
        }
        
        if (read(bt_socket, data_buffer, response->data_len) != response->data_len) {
            perror("Failed to read response data");
            return -1;
        }
    }
    
    return 0;
}

// ============ ТЕСТЫ ============

void test_ping() {
    print_log("=== TEST: PING ===");
    
    if (send_request(BTFS_OP_PING, NULL, 0) < 0) {
        print_log("FAILED: send_request");
        return;
    }
    
    btfs_response_t resp;
    if (receive_response(&resp, NULL, 0) < 0) {
        print_log("FAILED: receive_response");
        return;
    }
    
    if (resp.result == 0) {
        print_log("PASS: Ping successful");
    } else {
        print_log("FAILED: Ping returned error %d", resp.result);
    }
}

void test_getattr(const char *path) {
    print_log("=== TEST: GETATTR %s ===", path);
    
    btfs_getattr_req_t req;
    strncpy(req.path, path, BTFS_MAX_PATH - 1);
    
    if (send_request(BTFS_OP_GETATTR, &req, sizeof(req)) < 0) {
        print_log("FAILED: send_request");
        return;
    }
    
    btfs_response_t resp;
    btfs_attr_t attr;
    if (receive_response(&resp, &attr, sizeof(attr)) < 0) {
        print_log("FAILED: receive_response");
        return;
    }
    
    if (resp.result == 0) {
        print_log("PASS: size=%lu, mode=%o, uid=%u, gid=%u", 
                  attr.size, attr.mode, attr.uid, attr.gid);
    } else {
        print_log("FAILED: Error %d", resp.result);
    }
}

void test_readdir(const char *path) {
    print_log("=== TEST: READDIR %s ===", path);
    
    btfs_readdir_req_t req;
    strncpy(req.path, path, BTFS_MAX_PATH - 1);
    
    if (send_request(BTFS_OP_READDIR, &req, sizeof(req)) < 0) {
        print_log("FAILED: send_request");
        return;
    }
    
    btfs_response_t resp;
    char buffer[BTFS_MAX_DATA];
    if (receive_response(&resp, buffer, sizeof(buffer)) < 0) {
        print_log("FAILED: receive_response");
        return;
    }
    
    if (resp.result == 0) {
        print_log("PASS: Listed entries:");
        size_t offset = 0;
        while (offset < resp.data_len) {
            btfs_dirent_t *entry = (btfs_dirent_t*)(buffer + offset);
            printf("  - %s (type=%u, ino=%lu)\n", entry->name, entry->type, entry->ino);
            offset += sizeof(btfs_dirent_t) + entry->name_len + 1;
        }
    } else {
        print_log("FAILED: Error %d", resp.result);
    }
}

void test_create_read_write_delete() {
    print_log("=== TEST: CREATE/OPEN/WRITE/READ/CLOSE/UNLINK ===");
    
    const char *test_file = "test_file.txt";
    const char *test_data = "Hello, BTFS Server!";
    
    // CREATE
    btfs_create_req_t create_req;
    strncpy(create_req.path, test_file, BTFS_MAX_PATH - 1);
    create_req.mode = 0644;
    create_req.flags = 0;
    
    if (send_request(BTFS_OP_CREATE, &create_req, sizeof(create_req)) < 0) {
        print_log("FAILED: CREATE send");
        return;
    }
    
    btfs_response_t resp;
    if (receive_response(&resp, NULL, 0) < 0 || resp.result != 0) {
        print_log("FAILED: CREATE error %d", resp.result);
        return;
    }
    print_log("PASS: File created");
    
    // OPEN
    btfs_open_req_t open_req;
    strncpy(open_req.path, test_file, BTFS_MAX_PATH - 1);
    open_req.flags = O_RDWR;
    open_req.mode = 0;
    open_req.lock_type = BTFS_LOCK_NONE;
    
    if (send_request(BTFS_OP_OPEN, &open_req, sizeof(open_req)) < 0) {
        print_log("FAILED: OPEN send");
        return;
    }
    
    btfs_open_resp_t open_resp;
    if (receive_response(&resp, &open_resp, sizeof(open_resp)) < 0 || resp.result != 0) {
        print_log("FAILED: OPEN error %d", resp.result);
        return;
    }
    uint64_t file_handle = open_resp.file_handle;
    print_log("PASS: File opened, handle=%lu", file_handle);
    
    // WRITE
    btfs_write_req_t *write_req = malloc(sizeof(btfs_write_req_t) + strlen(test_data));
    write_req->file_handle = file_handle;
    write_req->offset = 0;
    write_req->size = strlen(test_data);
    memcpy(write_req->data, test_data, strlen(test_data));
    
    if (send_request(BTFS_OP_WRITE, write_req, sizeof(btfs_write_req_t) + strlen(test_data)) < 0) {
        print_log("FAILED: WRITE send");
        free(write_req);
        return;
    }
    free(write_req);
    
    btfs_write_resp_t write_resp;
    if (receive_response(&resp, &write_resp, sizeof(write_resp)) < 0 || resp.result != 0) {
        print_log("FAILED: WRITE error %d", resp.result);
        return;
    }
    print_log("PASS: Written %u bytes", write_resp.bytes_written);
    
    // READ
    btfs_read_req_t read_req;
    read_req.file_handle = file_handle;
    read_req.offset = 0;
    read_req.size = strlen(test_data);
    
    if (send_request(BTFS_OP_READ, &read_req, sizeof(read_req)) < 0) {
        print_log("FAILED: READ send");
        return;
    }
    
    char read_buffer[BTFS_MAX_DATA];
    if (receive_response(&resp, read_buffer, sizeof(read_buffer)) < 0 || resp.result != 0) {
        print_log("FAILED: READ error %d", resp.result);
        return;
    }
    
    btfs_read_resp_t *read_resp = (btfs_read_resp_t*)read_buffer;
    read_resp->data[read_resp->bytes_read] = '\0';
    
    if (strcmp(read_resp->data, test_data) == 0) {
        print_log("PASS: Read data matches: \"%s\"", read_resp->data);
    } else {
        print_log("FAILED: Read data mismatch");
    }
    
    // CLOSE
    btfs_close_req_t close_req;
    close_req.file_handle = file_handle;
    
    if (send_request(BTFS_OP_CLOSE, &close_req, sizeof(close_req)) < 0) {
        print_log("FAILED: CLOSE send");
        return;
    }
    
    if (receive_response(&resp, NULL, 0) < 0 || resp.result != 0) {
        print_log("FAILED: CLOSE error %d", resp.result);
        return;
    }
    print_log("PASS: File closed");
    
    // UNLINK
    btfs_unlink_req_t unlink_req;
    strncpy(unlink_req.path, test_file, BTFS_MAX_PATH - 1);
    
    if (send_request(BTFS_OP_UNLINK, &unlink_req, sizeof(unlink_req)) < 0) {
        print_log("FAILED: UNLINK send");
        return;
    }
    
    if (receive_response(&resp, NULL, 0) < 0 || resp.result != 0) {
        print_log("FAILED: UNLINK error %d", resp.result);
        return;
    }
    print_log("PASS: File deleted");
}

void test_mkdir_rmdir() {
    print_log("=== TEST: MKDIR/RMDIR ===");
    
    const char *test_dir = "test_directory";
    
    // MKDIR
    btfs_mkdir_req_t mkdir_req;
    strncpy(mkdir_req.path, test_dir, BTFS_MAX_PATH - 1);
    mkdir_req.mode = 0755;
    
    if (send_request(BTFS_OP_MKDIR, &mkdir_req, sizeof(mkdir_req)) < 0) {
        print_log("FAILED: MKDIR send");
        return;
    }
    
    btfs_response_t resp;
    if (receive_response(&resp, NULL, 0) < 0 || resp.result != 0) {
        print_log("FAILED: MKDIR error %d", resp.result);
        return;
    }
    print_log("PASS: Directory created");
    
    // RMDIR
    btfs_unlink_req_t rmdir_req;
    strncpy(rmdir_req.path, test_dir, BTFS_MAX_PATH - 1);
    
    if (send_request(BTFS_OP_RMDIR, &rmdir_req, sizeof(rmdir_req)) < 0) {
        print_log("FAILED: RMDIR send");
        return;
    }
    
    if (receive_response(&resp, NULL, 0) < 0 || resp.result != 0) {
        print_log("FAILED: RMDIR error %d", resp.result);
        return;
    }
    print_log("PASS: Directory deleted");
}

void test_locks() {
    print_log("=== TEST: FILE LOCKING ===");
    
    const char *test_file = "lock_test.txt";
    
    // CREATE файл
    btfs_create_req_t create_req;
    strncpy(create_req.path, test_file, BTFS_MAX_PATH - 1);
    create_req.mode = 0644;
    create_req.flags = 0;
    
    send_request(BTFS_OP_CREATE, &create_req, sizeof(create_req));
    btfs_response_t resp;
    receive_response(&resp, NULL, 0);
    
    // LOCK (READ)
    btfs_lock_req_t lock_req;
    strncpy(lock_req.path, test_file, BTFS_MAX_PATH - 1);
    lock_req.lock_type = BTFS_LOCK_READ;
    lock_req.timeout_sec = 0;
    
    if (send_request(BTFS_OP_LOCK, &lock_req, sizeof(lock_req)) < 0) {
        print_log("FAILED: LOCK send");
        return;
    }
    
    btfs_lock_resp_t lock_resp;
    if (receive_response(&resp, &lock_resp, sizeof(lock_resp)) < 0 || resp.result != 0) {
        print_log("FAILED: LOCK error %d", resp.result);
        return;
    }
    print_log("PASS: Lock acquired, lock_id=%u", lock_resp.lock_id);
    
    // UNLOCK
    if (send_request(BTFS_OP_UNLOCK, &lock_req, sizeof(lock_req)) < 0) {
        print_log("FAILED: UNLOCK send");
        return;
    }
    
    if (receive_response(&resp, NULL, 0) < 0 || resp.result != 0) {
        print_log("FAILED: UNLOCK error %d", resp.result);
        return;
    }
    print_log("PASS: Lock released");
    
    // Cleanup
    btfs_unlink_req_t unlink_req;
    strncpy(unlink_req.path, test_file, BTFS_MAX_PATH - 1);
    send_request(BTFS_OP_UNLINK, &unlink_req, sizeof(unlink_req));
    receive_response(&resp, NULL, 0);
}

// ============ ПОДКЛЮЧЕНИЕ ============

int connect_to_server(const char *server_mac) {
    bdaddr_t server_addr;
    str2ba(server_mac, &server_addr);
    uint8_t channel = 1;
    
    // Поиск через SDP
    uint32_t service_uuid_int[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
    uuid_t svc_uuid;
    sdp_uuid128_create(&svc_uuid, &service_uuid_int);
    
    sdp_session_t *session = sdp_connect(BDADDR_ANY, &server_addr, SDP_RETRY_IF_BUSY);
    if (session) {
        print_log("Searching for BTFS service via SDP...");
        sdp_list_t *search_list = sdp_list_append(NULL, &svc_uuid);
        sdp_list_t *response_list = NULL;
        uint32_t range = 0x0000ffff;
        sdp_list_t *attrid_list = sdp_list_append(NULL, &range);
        
        if (sdp_service_search_attr_req(session, search_list, SDP_ATTR_REQ_RANGE, 
                                       attrid_list, &response_list) == 0 && response_list) {
            sdp_list_t *r = response_list;
            for (; r; r = r->next) {
                sdp_record_t *rec = (sdp_record_t*)r->data;
                sdp_list_t *proto_list;
                
                if (sdp_get_access_protos(rec, &proto_list) == 0) {
                    sdp_list_t *p = proto_list;
                    for (; p; p = p->next) {
                        sdp_list_t *pds = (sdp_list_t*)p->data;
                        for (; pds; pds = pds->next) {
                            sdp_data_t *d = (sdp_data_t*)pds->data;
                            int proto = 0;
                            for (; d; d = d->next) {
                                switch (d->dtd) {
                                    case SDP_UUID16:
                                    case SDP_UUID32:
                                    case SDP_UUID128:
                                        proto = sdp_uuid_to_proto(&d->val.uuid);
                                        break;
                                    case SDP_UINT8:
                                        if (proto == RFCOMM_UUID) {
                                            channel = d->val.uint8;
                                            print_log("Found BTFS service on channel %d", channel);
                                        }
                                        break;
                                }
                            }
                        }
                    }
                    sdp_list_free(proto_list, 0);
                }
            }
            sdp_list_free(response_list, 0);
        }
        
        sdp_list_free(search_list, 0);
        sdp_list_free(attrid_list, 0);
        sdp_close(session);
    }
    
    // Подключение
    print_log("Connecting to %s on channel %d...", server_mac, channel);
    
    int sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    struct sockaddr_rc addr = {0};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = channel;
    bacpy(&addr.rc_bdaddr, &server_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }
    
    print_log("Connected to BTFS server successfully");
    client_id = getpid();
    
    return sock;
}

// ============ MAIN ============

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_mac> [test_name]\n", argv[0]);
        fprintf(stderr, "\nAvailable tests:\n");
        fprintf(stderr, "  ping           - Test server connectivity\n");
        fprintf(stderr, "  getattr        - Test file attributes\n");
        fprintf(stderr, "  readdir        - Test directory listing\n");
        fprintf(stderr, "  file_ops       - Test file operations\n");
        fprintf(stderr, "  dir_ops        - Test directory operations\n");
        fprintf(stderr, "  locks          - Test file locking\n");
        fprintf(stderr, "  all            - Run all tests\n");
        return 1;
    }
    
    const char *server_mac = argv[1];
    const char *test_name = (argc > 2) ? argv[2] : "all";
    
    print_log("===================================================");
    print_log("  BTFS TEST CLIENT");
    print_log("===================================================");
    print_log("Server MAC: %s", server_mac);
    print_log("Test: %s", test_name);
    print_log("===================================================\n");
    
    // Подключение
    bt_socket = connect_to_server(server_mac);
    if (bt_socket < 0) {
        print_log("ERROR: Failed to connect to server");
        return 1;
    }
    
    sleep(1);
    
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
    
    close(bt_socket);
    
    print_log("===================================================");
    print_log("Tests completed");
    print_log("===================================================");
    
    return 0;
}

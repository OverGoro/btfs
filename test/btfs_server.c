#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024

// Структура для передачи данных в поток клиента
typedef struct {
    int client_socket;
    struct sockaddr_rc client_addr;
    int client_id;
    time_t connect_time;
} client_info_t;

// Глобальные переменные
static int active_clients = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t server_running = 1;
static int server_socket_global = -1;

void print_timestamp() {
    time_t now;
    struct tm *timeinfo;
    char buffer[80];
    
    time(&now);
    timeinfo = localtime(&now);
    strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", timeinfo);
    printf("%s ", buffer);
}

// Обработчик сигналов для корректного завершения
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        print_timestamp();
        printf("\nReceived shutdown signal. Stopping server...\n");
        server_running = 0;
        
        // Закрыть server socket чтобы разблокировать accept()
        if (server_socket_global >= 0) {
            shutdown(server_socket_global, SHUT_RDWR);
            close(server_socket_global);
            server_socket_global = -1;
        }
    }
}

sdp_session_t *register_service(uint8_t rfcomm_channel)
{
    uint32_t service_uuid_int[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
    const char *service_name = "File Transfer Server";
    const char *service_dsc = "Multi-client server for file transfer";
    const char *service_prov = "YourDevice";

    uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid, svc_class_uuid;
    sdp_list_t *l2cap_list = 0, *rfcomm_list = 0, *root_list = 0,
               *proto_list = 0, *access_proto_list = 0, *svc_class_list = 0,
               *profile_list = 0;
    sdp_data_t *channel = 0;
    sdp_profile_desc_t profile;
    sdp_record_t *record = sdp_record_alloc();
    sdp_session_t *session = 0;

    print_timestamp();
    printf("Registering SDP service...\n");

    // Set service ID
    sdp_uuid128_create(&svc_uuid, &service_uuid_int);
    sdp_set_service_id(record, svc_uuid);

    // Set service class
    sdp_uuid32_create(&svc_class_uuid, SERIAL_PORT_SVCLASS_ID);
    svc_class_list = sdp_list_append(0, &svc_class_uuid);
    sdp_set_service_classes(record, svc_class_list);

    // Set the Bluetooth profile
    sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
    profile.version = 0x0100;
    profile_list = sdp_list_append(0, &profile);
    sdp_set_profile_descs(record, profile_list);

    // Make service publicly browsable
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root_list = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups(record, root_list);

    // Set L2CAP
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    l2cap_list = sdp_list_append(0, &l2cap_uuid);
    proto_list = sdp_list_append(0, l2cap_list);

    // Set RFCOMM
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    channel = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    rfcomm_list = sdp_list_append(0, &rfcomm_uuid);
    sdp_list_append(rfcomm_list, channel);
    sdp_list_append(proto_list, rfcomm_list);

    access_proto_list = sdp_list_append(0, proto_list);
    sdp_set_access_protos(record, access_proto_list);

    // Set name, provider, and description
    sdp_set_info_attr(record, service_name, service_prov, service_dsc);

    print_timestamp();
    printf("Connecting to local SDP server...\n");

    // Connect to local SDP server
    session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
    if (!session) {
        print_timestamp();
        fprintf(stderr, "ERROR: Failed to connect to SDP server\n");
        return NULL;
    }

    print_timestamp();
    printf("SDP server connected, registering service record...\n");

    if (sdp_record_register(session, record, 0) < 0) {
        print_timestamp();
        fprintf(stderr, "ERROR: Service registration failed\n");
        sdp_close(session);
        return NULL;
    }

    print_timestamp();
    printf("Service registered successfully:\n");
    printf("  Service Name: %s\n", service_name);
    printf("  Description: %s\n", service_dsc);
    printf("  RFCOMM Channel: %d\n", rfcomm_channel);

    // Cleanup
    sdp_data_free(channel);
    sdp_list_free(l2cap_list, 0);
    sdp_list_free(rfcomm_list, 0);
    sdp_list_free(root_list, 0);
    sdp_list_free(access_proto_list, 0);
    sdp_list_free(svc_class_list, 0);
    sdp_list_free(profile_list, 0);

    return session;
}

// Функция-обработчик клиента (выполняется в отдельном потоке)
void *client_handler(void *arg)
{
    client_info_t *client_info = (client_info_t *)arg;
    char buf[BUFFER_SIZE];
    char remote_addr[18];
    int bytes_read;
    time_t duration;

    ba2str(&client_info->client_addr.rc_bdaddr, remote_addr);

    print_timestamp();
    printf("=================================================\n");
    print_timestamp();
    printf("THREAD STARTED for Client #%d\n", client_info->client_id);
    print_timestamp();
    printf("Remote device: %s\n", remote_addr);
    print_timestamp();
    printf("Thread ID: %lu\n", pthread_self());
    print_timestamp();
    printf("=================================================\n");

    // Установить timeout на чтение для возможности проверки server_running
    struct timeval timeout;
    timeout.tv_sec = 2;  // 2 секунды таймаут
    timeout.tv_usec = 0;
    setsockopt(client_info->client_socket, SOL_SOCKET, SO_RCVTIMEO, 
               &timeout, sizeof(timeout));

    // Обработка клиента в цикле (persistent connection)
    while (server_running) {
        memset(buf, 0, sizeof(buf));
        
        print_timestamp();
        printf("[Client #%d] Reading data from %s...\n", 
               client_info->client_id, remote_addr);

        bytes_read = read(client_info->client_socket, buf, sizeof(buf) - 1);

        if (bytes_read > 0) {
            buf[bytes_read] = '\0';
            print_timestamp();
            printf("[Client #%d] Received %d bytes: [%s]\n", 
                   client_info->client_id, bytes_read, buf);

            // Echo back (опционально)
            const char *response = "ACK";
            write(client_info->client_socket, response, strlen(response));
            
        } else if (bytes_read == 0) {
            print_timestamp();
            printf("[Client #%d] Client %s closed connection\n", 
                   client_info->client_id, remote_addr);
            break;
        } else {
            // Проверить, это таймаут или реальная ошибка
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Таймаут - продолжить цикл для проверки server_running
                continue;
            } else {
                print_timestamp();
                fprintf(stderr, "[Client #%d] Read error: %s\n", 
                       client_info->client_id, strerror(errno));
                break;
            }
        }
    }

    // Вычислить время сессии
    duration = time(NULL) - client_info->connect_time;

    print_timestamp();
    printf("=================================================\n");
    print_timestamp();
    printf("[Client #%d] CLOSING CONNECTION\n", client_info->client_id);
    print_timestamp();
    printf("Session duration: %ld seconds\n", duration);
    print_timestamp();
    printf("=================================================\n");

    close(client_info->client_socket);

    // Обновить счетчик активных клиентов
    pthread_mutex_lock(&clients_mutex);
    active_clients--;
    print_timestamp();
    printf("Active clients: %d/%d\n", active_clients, MAX_CLIENTS);
    pthread_mutex_unlock(&clients_mutex);

    free(client_info);
    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    struct sockaddr_rc loc_addr = {0}, rem_addr = {0};
    char local_addr[18] = {0};
    int client_socket;
    socklen_t opt = sizeof(rem_addr);
    uint8_t port = 1;
    sdp_session_t *session;
    int connection_count = 0;
    pthread_t thread_id;

    // Установка обработчиков сигналов
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // Игнорировать SIGPIPE (может возникнуть при записи в закрытый сокет)
    signal(SIGPIPE, SIG_IGN);

    print_timestamp();
    printf("===================================================\n");
    printf("  BLUETOOTH MULTI-CLIENT FILE TRANSFER SERVER\n");
    printf("===================================================\n");
    print_timestamp();
    printf("Max concurrent clients: %d\n", MAX_CLIENTS);
    printf("\n");

    // Allocate socket
    server_socket_global = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (server_socket_global < 0) {
        print_timestamp();
        perror("ERROR: Socket creation failed");
        return -1;
    }

    print_timestamp();
    printf("RFCOMM socket created successfully (fd=%d)\n", server_socket_global);

    // Set socket options for reuse
    int reuse = 1;
    if (setsockopt(server_socket_global, SOL_SOCKET, SO_REUSEADDR, 
                   &reuse, sizeof(reuse)) < 0) {
        print_timestamp();
        perror("WARNING: setsockopt SO_REUSEADDR failed");
    }

    // Bind socket to port
    loc_addr.rc_family = AF_BLUETOOTH;
    loc_addr.rc_bdaddr = *BDADDR_ANY;
    loc_addr.rc_channel = port;

    if (bind(server_socket_global, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        print_timestamp();
        perror("ERROR: Bind failed");
        close(server_socket_global);
        return -1;
    }

    // Get local Bluetooth address
    struct sockaddr_rc actual_addr;
    socklen_t addr_len = sizeof(actual_addr);
    if (getsockname(server_socket_global, (struct sockaddr *)&actual_addr, &addr_len) == 0) {
        ba2str(&actual_addr.rc_bdaddr, local_addr);
        print_timestamp();
        printf("Socket bound to: %s, channel: %d\n", 
               local_addr, actual_addr.rc_channel);
    }

    // Register service with SDP
    session = register_service(port);
    if (!session) {
        close(server_socket_global);
        return -1;
    }

    // Put socket into listening mode (increased backlog for multiple clients)
    if (listen(server_socket_global, MAX_CLIENTS) < 0) {
        print_timestamp();
        perror("ERROR: Listen failed");
        sdp_close(session);
        close(server_socket_global);
        return -1;
    }

    print_timestamp();
    printf("Server is now listening on channel %d\n", port);
    printf("===================================================\n");
    printf("Waiting for incoming connections...\n");
    printf("Press Ctrl+C to stop the server\n");
    printf("===================================================\n\n");

    // Main accept loop
    while (server_running) {
        print_timestamp();
        printf("Waiting for next client connection...\n");

        // Accept connection
        client_socket = accept(server_socket_global, (struct sockaddr *)&rem_addr, &opt);
        
        if (client_socket < 0) {
            if (!server_running) {
                // Сервер остановлен - нормальный выход
                break;
            }
            if (errno == EINTR) {
                // Прерван сигналом - проверить server_running
                continue;
            }
            if (errno == EBADF || errno == EINVAL) {
                // Сокет закрыт - выход
                break;
            }
            print_timestamp();
            perror("ERROR: Accept failed");
            continue;
        }

        if (!server_running) {
            // Сервер остановлен после accept
            close(client_socket);
            break;
        }

        connection_count++;

        // Check if we can accept more clients
        pthread_mutex_lock(&clients_mutex);
        if (active_clients >= MAX_CLIENTS) {
            pthread_mutex_unlock(&clients_mutex);
            
            print_timestamp();
            printf("WARNING: Max clients reached (%d/%d). Rejecting connection.\n", 
                   active_clients, MAX_CLIENTS);
            
            const char *reject_msg = "Server full. Try again later.";
            write(client_socket, reject_msg, strlen(reject_msg));
            close(client_socket);
            continue;
        }
        active_clients++;
        pthread_mutex_unlock(&clients_mutex);

        // Prepare client info
        client_info_t *client_info = malloc(sizeof(client_info_t));
        if (!client_info) {
            print_timestamp();
            perror("ERROR: Memory allocation failed");
            close(client_socket);
            
            pthread_mutex_lock(&clients_mutex);
            active_clients--;
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        client_info->client_socket = client_socket;
        client_info->client_addr = rem_addr;
        client_info->client_id = connection_count;
        client_info->connect_time = time(NULL);

        char remote_addr[18];
        ba2str(&rem_addr.rc_bdaddr, remote_addr);
        
        print_timestamp();
        printf("=================================================\n");
        print_timestamp();
        printf("NEW CONNECTION #%d ACCEPTED\n", connection_count);
        print_timestamp();
        printf("Remote device: %s\n", remote_addr);
        print_timestamp();
        printf("Active clients: %d/%d\n", active_clients, MAX_CLIENTS);
        print_timestamp();
        printf("=================================================\n");

        // Create thread for client
        if (pthread_create(&thread_id, NULL, client_handler, client_info) != 0) {
            print_timestamp();
            perror("ERROR: Thread creation failed");
            close(client_socket);
            free(client_info);
            
            pthread_mutex_lock(&clients_mutex);
            active_clients--;
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }

        // Detach thread so resources are freed automatically
        pthread_detach(thread_id);

        print_timestamp();
        printf("Thread created for Client #%d (TID: %lu)\n\n", 
               connection_count, thread_id);
    }

    // Cleanup
    print_timestamp();
    printf("\n===================================================\n");
    printf("Shutting down server...\n");
    printf("===================================================\n");
    
    // Ждем завершения всех клиентских потоков
    print_timestamp();
    printf("Waiting for client threads to finish...\n");
    
    int wait_count = 0;
    while (active_clients > 0 && wait_count < 10) {
        sleep(1);
        wait_count++;
        print_timestamp();
        printf("Still active: %d clients...\n", active_clients);
    }
    
    if (server_socket_global >= 0) {
        print_timestamp();
        printf("Closing server socket...\n");
        close(server_socket_global);
        server_socket_global = -1;
    }
    
    print_timestamp();
    printf("Closing SDP session...\n");
    sdp_close(session);
    
    print_timestamp();
    printf("Server stopped gracefully.\n");
    printf("Total connections handled: %d\n", connection_count);

    return 0;
}

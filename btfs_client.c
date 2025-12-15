#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define RECONNECT_DELAY 5
#define MAX_NAME_LEN 248

void print_timestamp() {
    time_t now;
    struct tm *timeinfo;
    char buffer[80];
    
    time(&now);
    timeinfo = localtime(&now);
    strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", timeinfo);
    printf("%s ", buffer);
}

// Поиск RFCOMM канала через SDP по UUID сервиса
int sdp_search_rfcomm_channel(bdaddr_t *target, uuid_t *svc_uuid)
{
    sdp_list_t *response_list = NULL, *search_list, *attrid_list;
    sdp_session_t *session = 0;
    uint32_t range = 0x0000ffff;
    uint8_t channel = 0;
    int status;

    print_timestamp();
    printf("Starting SDP search on device...\n");

    // Подключение к SDP серверу на удаленном устройстве
    session = sdp_connect(BDADDR_ANY, target, SDP_RETRY_IF_BUSY);
    if (!session) {
        print_timestamp();
        fprintf(stderr, "ERROR: Failed to connect to SDP server: %s\n", strerror(errno));
        return -1;
    }

    print_timestamp();
    printf("Connected to remote SDP server\n");

    // Список UUID для поиска
    search_list = sdp_list_append(NULL, svc_uuid);
    
    // Атрибуты, которые нас интересуют
    attrid_list = sdp_list_append(NULL, &range);

    // Выполнение поиска
    print_timestamp();
    printf("Searching for service...\n");
    
    status = sdp_service_search_attr_req(session, search_list,
                                         SDP_ATTR_REQ_RANGE, attrid_list,
                                         &response_list);

    if (status < 0) {
        print_timestamp();
        fprintf(stderr, "ERROR: SDP search failed: %s\n", strerror(errno));
        sdp_close(session);
        return -1;
    }

    print_timestamp();
    printf("SDP search completed, processing results...\n");

    // Обработка результатов
    sdp_list_t *r = response_list;
    int found = 0;
    
    for (; r; r = r->next) {
        sdp_record_t *rec = (sdp_record_t*) r->data;
        sdp_list_t *proto_list;
        
        // Получить список протоколов
        if (sdp_get_access_protos(rec, &proto_list) == 0) {
            sdp_list_t *p = proto_list;
            
            // Пройти по протоколам
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
                                    found = 1;
                                    print_timestamp();
                                    printf("Found RFCOMM channel: %d\n", channel);
                                }
                                break;
                        }
                    }
                }
            }
            
            sdp_list_foreach(proto_list, (sdp_list_func_t)sdp_list_free, NULL);
            sdp_list_free(proto_list, NULL);
        }
        
        sdp_record_free(rec);
        
        if (found)
            break;
    }

    sdp_list_free(response_list, NULL);
    sdp_list_free(search_list, NULL);
    sdp_list_free(attrid_list, NULL);
    sdp_close(session);

    if (!found) {
        print_timestamp();
        printf("WARNING: Service not found via SDP\n");
        return -1;
    }

    return channel;
}

// Поиск устройства по имени
int find_device_by_name(char *target_name, bdaddr_t *target_addr)
{
    inquiry_info *devices = NULL;
    int max_rsp, num_rsp;
    int dev_id, sock, len, flags;
    int i;
    char addr[19] = {0};
    char name[MAX_NAME_LEN] = {0};
    int found = 0;

    print_timestamp();
    printf("=================================================\n");
    print_timestamp();
    printf("STARTING DEVICE DISCOVERY\n");
    print_timestamp();
    printf("=================================================\n");

    dev_id = hci_get_route(NULL);
    if (dev_id < 0) {
        print_timestamp();
        perror("ERROR: hci_get_route failed");
        return -1;
    }
    
    print_timestamp();
    printf("Using HCI device: hci%d\n", dev_id);

    sock = hci_open_dev(dev_id);
    if (sock < 0) {
        print_timestamp();
        perror("ERROR: hci_open_dev failed");
        return -1;
    }

    len = 8;
    max_rsp = 255;
    flags = IREQ_CACHE_FLUSH;
    devices = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));
    
    print_timestamp();
    printf("Starting inquiry scan (duration: ~%d seconds)...\n", (int)(len * 1.28));
    
    num_rsp = hci_inquiry(dev_id, len, max_rsp, NULL, &devices, flags);
    if (num_rsp < 0) {
        print_timestamp();
        perror("ERROR: hci_inquiry failed");
        free(devices);
        close(sock);
        return -1;
    }

    print_timestamp();
    printf("Inquiry completed. Found %d device(s)\n", num_rsp);
    print_timestamp();
    printf("-------------------------------------------------\n");

    for (i = 0; i < num_rsp; i++) {
        ba2str(&(devices[i].bdaddr), addr);
        memset(name, 0, sizeof(name));
        
        print_timestamp();
        printf("Device #%d: %s\n", i + 1, addr);
        
        if (hci_read_remote_name(sock, &(devices[i].bdaddr), sizeof(name), 
                                  name, 0) < 0) {
            strcpy(name, "[unknown]");
            print_timestamp();
            printf("  Name: %s (failed to read name)\n", name);
        } else {
            print_timestamp();
            printf("  Name: %s\n", name);
        }

        if (target_name && strstr(name, target_name)) {
            print_timestamp();
            printf("  *** MATCH FOUND! ***\n");
            bacpy(target_addr, &(devices[i].bdaddr));
            found = 1;
        }
        
        print_timestamp();
        printf("-------------------------------------------------\n");
    }

    free(devices);
    close(sock);
    
    if (found) {
        print_timestamp();
        printf("Target device '%s' found at %s\n", target_name, addr);
    } else {
        print_timestamp();
        printf("WARNING: Target device '%s' not found\n", target_name);
    }
    
    print_timestamp();
    printf("=================================================\n\n");

    return found ? 0 : -1;
}

int connect_to_server(bdaddr_t *server_addr, uint8_t channel)
{
    struct sockaddr_rc addr = {0};
    int s, status;
    char addr_str[18];

    print_timestamp();
    printf("=================================================\n");
    print_timestamp();
    printf("ESTABLISHING CONNECTION\n");
    print_timestamp();
    printf("=================================================\n");

    ba2str(server_addr, addr_str);
    
    print_timestamp();
    printf("Creating RFCOMM socket...\n");

    s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (s < 0) {
        print_timestamp();
        perror("ERROR: Socket creation failed");
        return -1;
    }

    print_timestamp();
    printf("Socket created successfully (fd=%d)\n", s);
    
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = channel;
    bacpy(&addr.rc_bdaddr, server_addr);

    print_timestamp();
    printf("Connection parameters:\n");
    print_timestamp();
    printf("  Remote address: %s\n", addr_str);
    print_timestamp();
    printf("  RFCOMM channel: %d\n", channel);
    
    print_timestamp();
    printf("Attempting to connect...\n");

    status = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    
    if (status == 0) {
        print_timestamp();
        printf("*** CONNECTION ESTABLISHED ***\n");
        print_timestamp();
        printf("=================================================\n\n");
        return s;
    } else {
        print_timestamp();
        fprintf(stderr, "ERROR: Connection failed: %s (errno=%d)\n", 
                strerror(errno), errno);
        print_timestamp();
        printf("=================================================\n\n");
        close(s);
        return -1;
    }
}

int main(int argc, char **argv)
{
    bdaddr_t server_addr;
    int sock = -1;
    char buf[1024];
    int bytes_read, bytes_written;
    char *server_mac = NULL;
    char *target_device = "Yurik";
    int channel = -1;
    int connection_count = 0;
    
    // UUID сервиса - соответствует тому, что в сервере
    uint32_t service_uuid_int[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
    uuid_t svc_uuid;

    print_timestamp();
    printf("#################################################\n");
    print_timestamp();
    printf("BLUETOOTH RFCOMM CLIENT WITH SDP\n");
    print_timestamp();
    printf("#################################################\n\n");

    if (argc > 1) {
        server_mac = argv[1];
    }
    
    if (argc > 2) {
        target_device = argv[2];
    }

    print_timestamp();
    printf("Configuration:\n");
    print_timestamp();
    printf("  Target device name: %s\n", target_device);
    print_timestamp();
    printf("  Reconnect delay: %d seconds\n", RECONNECT_DELAY);
    print_timestamp();
    printf("\n");

    // Создать UUID для поиска
    sdp_uuid128_create(&svc_uuid, &service_uuid_int);

    while (1) {
        if (sock < 0) {
            // Найти устройство
            if (server_mac) {
                print_timestamp();
                printf("Using provided MAC address: %s\n", server_mac);
                str2ba(server_mac, &server_addr);
            } else {
                if (find_device_by_name(target_device, &server_addr) != 0) {
                    print_timestamp();
                    printf("Device not found. Retrying in %d seconds...\n", RECONNECT_DELAY);
                    sleep(RECONNECT_DELAY);
                    continue;
                }
            }

            // Поиск RFCOMM канала через SDP
            channel = sdp_search_rfcomm_channel(&server_addr, &svc_uuid);
            
            if (channel < 0) {
                print_timestamp();
                printf("SDP search failed, trying default channel 1...\n");
                channel = 1;
            }

            // Подключение
            sock = connect_to_server(&server_addr, channel);
            
            if (sock < 0) {
                print_timestamp();
                printf("Failed to connect. Retrying in %d seconds...\n", RECONNECT_DELAY);
                sleep(RECONNECT_DELAY);
                continue;
            }
            
            connection_count++;
            print_timestamp();
            printf("This is connection #%d\n\n", connection_count);
        }

        // Отправка данных
        char addr_str[18];
        ba2str(&server_addr, addr_str);
        
        print_timestamp();
        printf("=================================================\n");
        print_timestamp();
        printf("SENDING DATA\n");
        print_timestamp();
        printf("=================================================\n");
        
        const char *message = "Hello from client with SDP!";
        print_timestamp();
        printf("Message: \"%s\"\n", message);
        
        bytes_written = write(sock, message, strlen(message));
        
        if (bytes_written < 0) {
            print_timestamp();
            fprintf(stderr, "ERROR: Write failed: %s\n", strerror(errno));
            close(sock);
            sock = -1;
            channel = -1;
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        print_timestamp();
        printf("Successfully sent %d bytes\n", bytes_written);
        print_timestamp();
        printf("=================================================\n\n");

        sleep(5);
    }

    if (sock >= 0) {
        close(sock);
    }
    
    return 0;
}

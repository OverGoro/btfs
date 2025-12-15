#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>

#define MAX_DEVICES 255
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

int find_device(char *target_name, bdaddr_t *target_addr)
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
    
    print_timestamp();
    printf("HCI socket opened (fd=%d)\n", sock);

    len = 8;  // inquiry duration: 1.28 * 8 = ~10 seconds
    max_rsp = MAX_DEVICES;
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

int auto_pair(bdaddr_t *bdaddr) {
    int dev_id = hci_get_route(NULL);
    int sock = hci_open_dev(dev_id);
    
    if (sock < 0) {
        print_timestamp();
        perror("WARNING: Failed to open HCI device for pairing");
        return -1;
    }
    
    print_timestamp();
    printf("Attempting to authenticate link...\n");
    
    // Попытка создать аутентифицированное соединение
    uint16_t handle;
    if (hci_create_connection(sock, bdaddr, htobs(0xcc18), 0, 0, &handle, 25000) < 0) {
        print_timestamp();
        perror("WARNING: Authentication link failed");
        close(sock);
        return -1;
    }
    
    print_timestamp();
    printf("Authentication successful\n");
    
    close(sock);
    return 0;
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

    // Allocate socket
    s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (s < 0) {
        print_timestamp();
        perror("ERROR: Socket creation failed");
        return -1;
    }

    // Установить низкий уровень безопасности (без сопряжения)
    int security_level = BT_SECURITY_LOW;
    if (setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &security_level, sizeof(security_level)) < 0) {
        print_timestamp();
        perror("WARNING: Failed to set security level");
    } else {
        print_timestamp();
        printf("Security level set to LOW\n");
    }

    print_timestamp();
    printf("Socket created successfully (fd=%d)\n", s);
    
    // Set connection parameters
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
    printf("  Protocol: RFCOMM\n");
    
    print_timestamp();
    printf("Attempting to connect...\n");

    // Connect to server
    status = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    
    if (status == 0) {
        print_timestamp();
        printf("*** CONNECTION ESTABLISHED ***\n");
        print_timestamp();
        printf("Connected to %s on channel %d\n", addr_str, channel);
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
    char *target_device = "Yurik";  // Имя устройства-сервера
    char *server_mac = NULL;
    uint8_t channel = 1;
    int connection_count = 0;
    int reconnect_attempts = 0;

    print_timestamp();
    printf("#################################################\n");
    print_timestamp();
    printf("BLUETOOTH RFCOMM CLIENT STARTING\n");
    print_timestamp();
    printf("#################################################\n\n");

    // Parse command line arguments
    if (argc > 1) {
        server_mac = argv[1];
        print_timestamp();
        printf("Using MAC address from command line: %s\n", server_mac);
    }
    
    if (argc > 2) {
        target_device = argv[2];
    }

    print_timestamp();
    printf("Configuration:\n");
    print_timestamp();
    printf("  Target device name: %s\n", target_device);
    print_timestamp();
    printf("  RFCOMM channel: %d\n", channel);
    print_timestamp();
    printf("  Reconnect delay: %d seconds\n", RECONNECT_DELAY);
    print_timestamp();
    printf("\n");

    while (1) {
        // Find and connect to server if not connected
        if (sock < 0) {
            reconnect_attempts++;
            
            print_timestamp();
            printf("Connection attempt #%d\n", reconnect_attempts);
            
            // If MAC address provided, use it directly
            if (server_mac) {
                print_timestamp();
                printf("Using provided MAC address: %s\n", server_mac);
                str2ba(server_mac, &server_addr);
                
                // Попытка автоматического сопряжения (опционально)
                // auto_pair(&server_addr);
                
                sock = connect_to_server(&server_addr, channel);
            } else {
                // Otherwise, scan for device
                if (find_device(target_device, &server_addr) == 0) {
                    sock = connect_to_server(&server_addr, channel);
                } else {
                    print_timestamp();
                    printf("Device not found during scan\n");
                }
            }
            
            if (sock < 0) {
                print_timestamp();
                printf("Failed to connect. Retrying in %d seconds...\n", RECONNECT_DELAY);
                print_timestamp();
                printf("\n");
                sleep(RECONNECT_DELAY);
                continue;
            }
            
            connection_count++;
            reconnect_attempts = 0;
            
            print_timestamp();
            printf("This is connection #%d\n\n", connection_count);
        }

        // Send data
        char addr_str[18];
        ba2str(&server_addr, addr_str);
        
        print_timestamp();
        printf("=================================================\n");
        print_timestamp();
        printf("SENDING DATA\n");
        print_timestamp();
        printf("=================================================\n");
        
        const char *message = "Hello from client!";
        print_timestamp();
        printf("Message: \"%s\"\n", message);
        print_timestamp();
        printf("Message length: %lu bytes\n", strlen(message));
        print_timestamp();
        printf("Target: %s\n", addr_str);
        
        bytes_written = write(sock, message, strlen(message));
        
        if (bytes_written < 0) {
            print_timestamp();
            fprintf(stderr, "ERROR: Write failed: %s (errno=%d)\n", 
                    strerror(errno), errno);
            print_timestamp();
            printf("Connection lost. Closing socket.\n");
            close(sock);
            sock = -1;
            print_timestamp();
            printf("=================================================\n\n");
            print_timestamp();
            printf("Attempting to reconnect...\n\n");
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        print_timestamp();
        printf("Successfully sent %d bytes\n", bytes_written);
        print_timestamp();
        printf("=================================================\n\n");

        // Try to read response (optional)
        print_timestamp();
        printf("Waiting for response (with timeout)...\n");
        
        // Set socket timeout
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        memset(buf, 0, sizeof(buf));
        bytes_read = read(sock, buf, sizeof(buf));
        
        if (bytes_read > 0) {
            print_timestamp();
            printf("Received response: %d bytes\n", bytes_read);
            print_timestamp();
            printf("Data: [%s]\n", buf);
        } else if (bytes_read == 0) {
            print_timestamp();
            printf("Server closed connection\n");
            close(sock);
            sock = -1;
            print_timestamp();
            printf("Attempting to reconnect...\n\n");
            sleep(RECONNECT_DELAY);
            continue;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                print_timestamp();
                printf("No response (timeout)\n");
            } else {
                print_timestamp();
                fprintf(stderr, "ERROR: Read failed: %s (errno=%d)\n", 
                        strerror(errno), errno);
                close(sock);
                sock = -1;
                print_timestamp();
                printf("Connection lost. Attempting to reconnect...\n\n");
                sleep(RECONNECT_DELAY);
                continue;
            }
        }

        print_timestamp();
        printf("\n");
        
        // Wait before next iteration
        print_timestamp();
        printf("Waiting 5 seconds before next message...\n");
        print_timestamp();
        printf("\n");
        sleep(5);
    }

    if (sock >= 0) {
        print_timestamp();
        printf("Closing connection...\n");
        close(sock);
    }
    
    print_timestamp();
    printf("Client terminated. Total connections: %d\n", connection_count);
    
    return 0;
}

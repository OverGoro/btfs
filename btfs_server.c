#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <time.h>


void print_timestamp() {
    time_t now;
    struct tm *timeinfo;
    char buffer[80];
    
    time(&now);
    timeinfo = localtime(&now);
    strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", timeinfo);
    printf("%s ", buffer);
}


sdp_session_t *register_service(uint8_t rfcomm_channel)
{
    uint32_t service_uuid_int[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
    const char *service_name = "File Transfer Server";
    const char *service_dsc = "Server for file transfer";
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


int main(int argc, char **argv)
{
    struct sockaddr_rc loc_addr = {0}, rem_addr = {0};
    char buf[1024] = {0};
    char local_addr[18] = {0};
    char remote_addr[18] = {0};
    int s, client, bytes_read;
    socklen_t opt = sizeof(rem_addr);
    uint8_t port = 1;
    sdp_session_t *session;
    int connection_count = 0;

    print_timestamp();
    printf("Starting Bluetooth File Transfer Server...\n");

    // Allocate socket
    s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (s < 0) {
        print_timestamp();
        perror("ERROR: Socket creation failed");
        return -1;
    }

    // int security_level = BT_SECURITY_LOW;
    // if (setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &security_level, sizeof(security_level)) < 0) {
    //     perror("WARNING: Failed to set security level");
    // }
    
    print_timestamp();
    printf("RFCOMM socket created successfully (fd=%d)\n", s);

    // Bind socket to port 1
    loc_addr.rc_family = AF_BLUETOOTH;
    loc_addr.rc_bdaddr = *BDADDR_ANY;
    loc_addr.rc_channel = port;
    
    if (bind(s, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        print_timestamp();
        perror("ERROR: Bind failed");
        close(s);
        return -1;
    }

    // Get local Bluetooth address
    struct sockaddr_rc actual_addr;
    socklen_t addr_len = sizeof(actual_addr);
    if (getsockname(s, (struct sockaddr *)&actual_addr, &addr_len) == 0) {
        ba2str(&actual_addr.rc_bdaddr, local_addr);
        print_timestamp();
        printf("Socket bound to local address: %s, channel: %d\n", 
               local_addr, actual_addr.rc_channel);
    }

    // Register service with SDP
    session = register_service(port);
    if (!session) {
        close(s);
        return -1;
    }

    // Put socket into listening mode
    if (listen(s, 1) < 0) {
        print_timestamp();
        perror("ERROR: Listen failed");
        sdp_close(session);
        close(s);
        return -1;
    }

    print_timestamp();
    printf("Server is now listening on channel %d\n", port);
    printf("=================================================\n");
    printf("Waiting for incoming connections...\n");
    printf("=================================================\n");

    while (1) {
        // Accept connection
        print_timestamp();
        printf("Waiting for client connection...\n");
        
        client = accept(s, (struct sockaddr *)&rem_addr, &opt);
        if (client < 0) {
            print_timestamp();
            perror("ERROR: Accept failed");
            continue;
        }

        connection_count++;
        ba2str(&rem_addr.rc_bdaddr, remote_addr);
        
        print_timestamp();
        printf("=================================================\n");
        print_timestamp();
        printf("CONNECTION #%d ESTABLISHED\n", connection_count);
        print_timestamp();
        printf("Remote device address: %s\n", remote_addr);
        print_timestamp();
        printf("Remote device channel: %d\n", rem_addr.rc_channel);
        print_timestamp();
        printf("Client socket fd: %d\n", client);
        print_timestamp();
        printf("=================================================\n");

        // Handle file transfer here
        memset(buf, 0, sizeof(buf));
        print_timestamp();
        printf("Reading data from %s...\n", remote_addr);
        
        bytes_read = read(client, buf, sizeof(buf));
        if (bytes_read > 0) {
            print_timestamp();
            printf("Received %d bytes from %s\n", bytes_read, remote_addr);
            print_timestamp();
            printf("Data: [%s]\n", buf);
        } else if (bytes_read == 0) {
            print_timestamp();
            printf("Client %s closed connection\n", remote_addr);
        } else {
            print_timestamp();
            perror("ERROR: Read failed");
        }

        // Close client connection
        print_timestamp();
        printf("Closing connection with %s\n", remote_addr);
        close(client);
        
        print_timestamp();
        printf("Connection #%d closed\n", connection_count);
        printf("=================================================\n\n");
    }

    // Cleanup
    print_timestamp();
    printf("Shutting down server...\n");
    sdp_close(session);
    close(s);
    
    print_timestamp();
    printf("Server stopped. Total connections handled: %d\n", connection_count);
    
    return 0;
}

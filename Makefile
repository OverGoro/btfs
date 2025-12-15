# BTFS Makefile

CC = gcc
CFLAGS = -Wall -O2 -pthread
LDFLAGS = -lbluetooth -pthread
KDIR = /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Kernel modules
obj-m += btfs_server_fs.o
obj-m += btfs_client_fs.o

.PHONY: all clean build-server build-client load-server load-client \
        start-server start-client stop-server stop-client \
        unload-server unload-client

# ============ СБОРКА ============

all: build-server build-client

build-server: btfs_server_daemon btfs_server_fs.ko

build-client: btfs_client_daemon btfs_client_fs.ko

btfs_server_daemon: btfs_server_daemon.c btfs_protocol.h
	$(CC) $(CFLAGS) -o btfs_server_daemon btfs_server_daemon.c $(LDFLAGS)

btfs_client_daemon: btfs_client_daemon.c btfs_protocol.h
	$(CC) $(CFLAGS) -o btfs_client_daemon btfs_client_daemon.c $(LDFLAGS)

btfs_server_fs.ko: btfs_server_fs.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

btfs_client_fs.ko: btfs_client_fs.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ============ СЕРВЕР ============

load-server: btfs_server_fs.ko
	insmod btfs_server_fs.ko
	mkdir -p /srv/btfs_shared /mnt/btfs_server
	mount -t btfs_server -o backing_dir=/srv/btfs_shared none /mnt/btfs_server

start-server: btfs_server_daemon
	./btfs_server_daemon /srv/btfs_shared &

stop-server:
	-pkill -f btfs_server_daemon

unload-server:
	-umount /mnt/btfs_server
	-rmmod btfs_server_fs

# ============ КЛИЕНТ ============

load-client: btfs_client_fs.ko
	insmod btfs_client_fs.ko
	mkdir -p /mnt/btfs_client
	mount -t btfs none /mnt/btfs_client

start-client: btfs_client_daemon
	@if [ -z "$(MAC)" ]; then echo "Usage: make start-client MAC=XX:XX:XX:XX:XX:XX"; exit 1; fi
	./btfs_client_daemon $(MAC) &

stop-client:
	-pkill -f btfs_client_daemon

unload-client:
	-umount /mnt/btfs_client
	-rmmod btfs_client_fs

# ============ ОЧИСТКА ============

clean:
	rm -f btfs_server_daemon btfs_client_daemon
	rm -f *.o .*.o *.ko *.mod.* *.mod .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions

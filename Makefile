obj-m += btfs_client_fs.o btfs_server_fs.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: modules daemons

modules:
	make -C $(KERNEL_DIR) M=$(PWD) modules

daemons:
	gcc -o btfs_client_daemon btfs_client_daemon.c -lbluetooth -lpthread
	gcc -o btfs_server_daemon btfs_server_daemon.c -lbluetooth -lpthread

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f btfs_client_daemon btfs_server_daemon

install_client:
	insmod btfs_client_fs.ko
	
install_server:
	insmod btfs_server_fs.ko

uninstall:
	-rmmod btfs_client_fs
	-rmmod btfs_server_fs

.PHONY: all modules daemons clean install_client install_server uninstall

// SPDX-License-Identifier: GPL-2.0
/*
 * BTFS Server Daemon - Bluetooth File System Server
 * Copyright (C) 2024 BTFS Team
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <dirent.h>
#include <limits.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include "btfs_protocol.h"

#define MAX_CLIENTS		10
#define MAX_OPEN_FILES		1000
#define LOCK_CHECK_INTERVAL	5
#define CLIENT_TIMEOUT		60

typedef struct file_lock {
	uint32_t lock_id;
	char path[BTFS_MAX_PATH];
	uint32_t client_id;
	btfs_lock_type_t type;
	time_t acquired_time;
	time_t timeout_time;
	struct file_lock *next;
} file_lock_t;

typedef struct {
	uint64_t handle;
	int fd;
	char path[BTFS_MAX_PATH];
	uint32_t client_id;
	uint32_t flags;
	uint32_t lock_id;
	time_t last_access;
} open_file_t;

typedef struct {
	int socket;
	uint32_t client_id;
	struct sockaddr_rc addr;
	pthread_t thread;
	time_t connect_time;
	time_t last_activity;
	int active;
} client_info_t;

static char *base_path;
static client_info_t clients[MAX_CLIENTS];
static open_file_t open_files[MAX_OPEN_FILES];
static file_lock_t *locks_head;
static pthread_mutex_t locks_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_client_id = 1;
static uint64_t next_file_handle = 1;
static uint32_t next_lock_id = 1;
static volatile int server_running = 1;

static void log_msg(const char *format, ...)
{
	va_list args;
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	char timestamp[64];

	strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", t);
	printf("%s ", timestamp);
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
	fflush(stdout);
}

static int build_full_path(char *full_path, size_t size, const char *rel_path)
{
	return snprintf(full_path, size, "%s/%s", base_path, rel_path) >= (int)size
		? -ENAMETOOLONG : 0;
}

/* ========== File tracking ========== */

static open_file_t *find_open_file(uint64_t handle, uint32_t client_id)
{
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (open_files[i].handle == handle &&
		    open_files[i].client_id == client_id) {
			open_files[i].last_access = time(NULL);
			return &open_files[i];
		}
	}
	return NULL;
}

static file_lock_t *find_lock(const char *path, uint32_t client_id)
{
	file_lock_t *lock = locks_head;

	while (lock) {
		if (strcmp(lock->path, path) == 0 && lock->client_id == client_id)
			return lock;
		lock = lock->next;
	}
	return NULL;
}

/* ========== Lock manager ========== */

static int check_lock_conflict(const char *path, btfs_lock_type_t new_type,
				uint32_t client_id)
{
	file_lock_t *lock;

	pthread_mutex_lock(&locks_mutex);
	for (lock = locks_head; lock; lock = lock->next) {
		if (strcmp(lock->path, path) == 0) {
			if ((new_type == BTFS_LOCK_READ && lock->type == BTFS_LOCK_READ) ||
			    lock->client_id == client_id) {
				pthread_mutex_unlock(&locks_mutex);
				return 0;
			}
			pthread_mutex_unlock(&locks_mutex);
			return -EAGAIN;
		}
	}
	pthread_mutex_unlock(&locks_mutex);
	return 0;
}

static uint32_t acquire_lock(const char *path, btfs_lock_type_t type,
			      uint32_t client_id, uint32_t timeout_sec)
{
	file_lock_t *lock;
	uint32_t id;

	pthread_mutex_lock(&locks_mutex);

	lock = find_lock(path, client_id);
	if (lock) {
		lock->type = type;
		lock->acquired_time = time(NULL);
		lock->timeout_time = timeout_sec ? lock->acquired_time + timeout_sec : 0;
		id = lock->lock_id;
		pthread_mutex_unlock(&locks_mutex);
		return id;
	}

	lock = malloc(sizeof(*lock));
	if (!lock) {
		pthread_mutex_unlock(&locks_mutex);
		return 0;
	}

	lock->lock_id = next_lock_id++;
	strncpy(lock->path, path, BTFS_MAX_PATH - 1);
	lock->path[BTFS_MAX_PATH - 1] = '\0';
	lock->client_id = client_id;
	lock->type = type;
	lock->acquired_time = time(NULL);
	lock->timeout_time = timeout_sec ? lock->acquired_time + timeout_sec : 0;
	lock->next = locks_head;
	locks_head = lock;

	id = lock->lock_id;
	pthread_mutex_unlock(&locks_mutex);
	return id;
}

static int release_lock(uint32_t lock_id, uint32_t client_id)
{
	file_lock_t *lock, *prev = NULL;

	pthread_mutex_lock(&locks_mutex);
	for (lock = locks_head; lock; prev = lock, lock = lock->next) {
		if (lock->lock_id == lock_id && lock->client_id == client_id) {
			if (prev)
				prev->next = lock->next;
			else
				locks_head = lock->next;
			free(lock);
			pthread_mutex_unlock(&locks_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&locks_mutex);
	return -ENOENT;
}

static void release_client_locks(uint32_t client_id)
{
	file_lock_t *lock, *prev = NULL;

	pthread_mutex_lock(&locks_mutex);
	lock = locks_head;
	while (lock) {
		if (lock->client_id == client_id) {
			file_lock_t *to_free = lock;
			if (prev) {
				prev->next = lock->next;
				lock = lock->next;
			} else {
				locks_head = lock->next;
				lock = locks_head;
			}
			free(to_free);
		} else {
			prev = lock;
			lock = lock->next;
		}
	}
	pthread_mutex_unlock(&locks_mutex);
}

static void *lock_timeout_thread(void *arg)
{
	while (server_running) {
		sleep(LOCK_CHECK_INTERVAL);

		time_t now = time(NULL);
		file_lock_t *lock, *prev = NULL;

		pthread_mutex_lock(&locks_mutex);
		lock = locks_head;
		while (lock) {
			if (lock->timeout_time > 0 && now >= lock->timeout_time) {
				file_lock_t *to_free = lock;
				if (prev) {
					prev->next = lock->next;
					lock = lock->next;
				} else {
					locks_head = lock->next;
					lock = locks_head;
				}
				free(to_free);
			} else {
				prev = lock;
				lock = lock->next;
			}
		}
		pthread_mutex_unlock(&locks_mutex);
	}
	return NULL;
}

/* ========== File operations ========== */

static uint64_t handle_open(uint32_t client_id, const char *rel_path,
			     uint32_t flags, uint32_t mode, uint32_t lock_type)
{
	char full_path[PATH_MAX];
	int posix_flags, fd, slot;
	uint32_t lock_id = 0;
	uint64_t handle;

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) {
		errno = ENAMETOOLONG;
		return 0;
	}

	if (lock_type != BTFS_LOCK_NONE &&
	    check_lock_conflict(rel_path, lock_type, client_id) < 0) {
		errno = EAGAIN;
		return 0;
	}

	posix_flags = (flags & O_ACCMODE) == O_WRONLY ? O_WRONLY :
		      (flags & O_ACCMODE) == O_RDWR ? O_RDWR : O_RDONLY;

	if (flags & O_APPEND) posix_flags |= O_APPEND;
	if (flags & O_TRUNC)  posix_flags |= O_TRUNC;
	if (flags & O_CREAT)  posix_flags |= O_CREAT;
	if (flags & O_EXCL)   posix_flags |= O_EXCL;
	if (flags & O_SYNC)   posix_flags |= O_SYNC;

	fd = open(full_path, posix_flags, mode ?: 0644);
	if (fd < 0) {
		errno = errno;
		return 0;
	}

	if (lock_type != BTFS_LOCK_NONE) {
		lock_id = acquire_lock(rel_path, lock_type, client_id, 0);
		if (!lock_id) {
			close(fd);
			errno = ENOMEM;
			return 0;
		}
	}

	pthread_mutex_lock(&files_mutex);
	slot = -1;
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (open_files[i].handle == 0) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		pthread_mutex_unlock(&files_mutex);
		close(fd);
		if (lock_id)
			release_lock(lock_id, client_id);
		errno = ENFILE;
		return 0;
	}

	open_files[slot].handle = next_file_handle++;
	open_files[slot].fd = fd;
	strncpy(open_files[slot].path, rel_path, BTFS_MAX_PATH - 1);
	open_files[slot].path[BTFS_MAX_PATH - 1] = '\0';
	open_files[slot].client_id = client_id;
	open_files[slot].flags = posix_flags;
	open_files[slot].lock_id = lock_id;
	open_files[slot].last_access = time(NULL);

	handle = open_files[slot].handle;
	pthread_mutex_unlock(&files_mutex);

	return handle;
}

static int handle_close(uint32_t client_id, uint64_t file_handle)
{
	pthread_mutex_lock(&files_mutex);
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (open_files[i].handle == file_handle &&
		    open_files[i].client_id == client_id) {
			close(open_files[i].fd);
			if (open_files[i].lock_id)
				release_lock(open_files[i].lock_id, client_id);
			memset(&open_files[i], 0, sizeof(open_file_t));
			pthread_mutex_unlock(&files_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&files_mutex);
	return -EBADF;
}

static void close_client_files(uint32_t client_id)
{
	pthread_mutex_lock(&files_mutex);
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (open_files[i].handle && open_files[i].client_id == client_id) {
			close(open_files[i].fd);
			if (open_files[i].lock_id)
				release_lock(open_files[i].lock_id, client_id);
			memset(&open_files[i], 0, sizeof(open_file_t));
		}
	}
	pthread_mutex_unlock(&files_mutex);
}

static ssize_t handle_read(uint32_t client_id, uint64_t file_handle,
			   uint64_t offset, uint32_t size, char *buffer)
{
	open_file_t *file;
	int fd;
	ssize_t ret;

	pthread_mutex_lock(&files_mutex);
	file = find_open_file(file_handle, client_id);
	fd = file ? file->fd : -1;
	pthread_mutex_unlock(&files_mutex);

	if (fd < 0)
		return -EBADF;

	ret = pread(fd, buffer, size, offset);
	return ret < 0 ? -errno : ret;
}

static ssize_t handle_write(uint32_t client_id, uint64_t file_handle,
			    uint64_t offset, const char *data, uint32_t size)
{
	open_file_t *file;
	int fd;
	ssize_t ret;

	pthread_mutex_lock(&files_mutex);
	file = find_open_file(file_handle, client_id);
	fd = file ? file->fd : -1;
	pthread_mutex_unlock(&files_mutex);

	if (fd < 0)
		return -EBADF;

	ret = pwrite(fd, data, size, offset);
	return ret < 0 ? -errno : ret;
}

static int handle_getattr(const char *rel_path, btfs_attr_t *attr)
{
	char full_path[PATH_MAX];
	struct stat st;

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;

	if (lstat(full_path, &st) < 0)
		return -errno;

	memset(attr, 0, sizeof(*attr));
	attr->ino = st.st_ino;
	attr->size = st.st_size;
	attr->blocks = st.st_blocks;
	attr->mode = st.st_mode;
	attr->nlink = st.st_nlink;
	attr->uid = st.st_uid;
	attr->gid = st.st_gid;
	attr->atime_sec = st.st_atim.tv_sec;
	attr->atime_nsec = st.st_atim.tv_nsec;
	attr->mtime_sec = st.st_mtim.tv_sec;
	attr->mtime_nsec = st.st_mtim.tv_nsec;
	attr->ctime_sec = st.st_ctim.tv_sec;
	attr->ctime_nsec = st.st_ctim.tv_nsec;

	return 0;
}

static int handle_readdir(const char *rel_path, char *buffer,
			  size_t buffer_size, uint64_t *offset_out)
{
	char full_path[PATH_MAX];
	DIR *dir;
	struct dirent *entry;
	size_t used = 0;
	int count = 0;

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;

	dir = opendir(full_path);
	if (!dir)
		return -errno;

	while ((entry = readdir(dir))) {
		size_t name_len, entry_size;
		btfs_dirent_t *dirent;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		name_len = strlen(entry->d_name);
		entry_size = sizeof(btfs_dirent_t) + name_len + 1;

		if (used + entry_size > buffer_size)
			break;

		dirent = (btfs_dirent_t *)(buffer + used);
		dirent->ino = entry->d_ino;
		dirent->type = entry->d_type;
		dirent->name_len = name_len;
		memcpy(dirent->name, entry->d_name, name_len + 1);

		used += entry_size;
		count++;
	}

	closedir(dir);
	*offset_out = used;
	return count;
}

static int handle_mkdir(const char *rel_path, uint32_t mode)
{
	char full_path[PATH_MAX];

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;
	return mkdir(full_path, mode) < 0 ? -errno : 0;
}

static int handle_rmdir(const char *rel_path)
{
	char full_path[PATH_MAX];

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;
	return rmdir(full_path) < 0 ? -errno : 0;
}

static int handle_create(const char *rel_path, uint32_t mode, uint32_t flags)
{
	char full_path[PATH_MAX];
	int fd;

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;

	fd = open(full_path, O_CREAT | O_EXCL | O_WRONLY | flags, mode);
	if (fd < 0)
		return -errno;

	close(fd);
	return 0;
}

static int handle_unlink(const char *rel_path)
{
	char full_path[PATH_MAX];

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;
	return unlink(full_path) < 0 ? -errno : 0;
}

static int handle_rename(const char *oldpath, const char *newpath)
{
	char old_full[PATH_MAX], new_full[PATH_MAX];

	if (build_full_path(old_full, sizeof(old_full), oldpath) < 0 ||
	    build_full_path(new_full, sizeof(new_full), newpath) < 0)
		return -ENAMETOOLONG;

	return rename(old_full, new_full) < 0 ? -errno : 0;
}

static int handle_truncate(const char *rel_path, uint64_t size)
{
	char full_path[PATH_MAX];

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;
	return truncate(full_path, size) < 0 ? -errno : 0;
}

static int handle_chmod(const char *rel_path, uint32_t mode)
{
	char full_path[PATH_MAX];

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;
	return chmod(full_path, mode) < 0 ? -errno : 0;
}

static int handle_chown(const char *rel_path, uint32_t uid, uint32_t gid)
{
	char full_path[PATH_MAX];

	if (build_full_path(full_path, sizeof(full_path), rel_path) < 0)
		return -ENAMETOOLONG;
	return chown(full_path, uid, gid) < 0 ? -errno : 0;
}

static int handle_symlink(const char *target, const char *linkpath)
{
	char link_full[PATH_MAX];

	if (build_full_path(link_full, sizeof(link_full), linkpath) < 0)
		return -ENAMETOOLONG;
	return symlink(target, link_full) < 0 ? -errno : 0;
}

static int handle_readlink(const char *linkpath, char *buffer, size_t bufsize)
{
	char link_full[PATH_MAX];
	ssize_t len;

	if (build_full_path(link_full, sizeof(link_full), linkpath) < 0)
		return -ENAMETOOLONG;

	len = readlink(link_full, buffer, bufsize - 1);
	if (len < 0)
		return -errno;

	buffer[len] = '\0';
	return 0;
}

static int handle_fsync(uint32_t client_id, uint64_t file_handle)
{
	open_file_t *file;
	int fd, ret;

	pthread_mutex_lock(&files_mutex);
	file = find_open_file(file_handle, client_id);
	fd = file ? file->fd : -1;
	pthread_mutex_unlock(&files_mutex);

	if (fd < 0)
		return -EBADF;

	ret = fsync(fd);
	return ret < 0 ? -errno : 0;
}

/* ========== Protocol ========== */

static int send_response(int sock, uint32_t sequence, uint32_t opcode,
			 int32_t result, const void *data, uint32_t data_len)
{
	btfs_response_t resp = {
		.opcode = opcode,
		.sequence = sequence,
		.result = result,
		.data_len = data_len,
	};
	size_t sent;
	ssize_t n;

	sent = 0;
	while (sent < sizeof(resp)) {
		n = write(sock, (char *)&resp + sent, sizeof(resp) - sent);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += n;
	}

	if (data && data_len > 0) {
		sent = 0;
		while (sent < data_len) {
			n = write(sock, (char *)data + sent, data_len - sent);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			sent += n;
		}
	}

	return 0;
}

static int receive_request(int sock, btfs_header_t *header,
			   char *data_buffer, size_t buffer_size)
{
	ssize_t n;

	n = recv(sock, header, sizeof(*header), MSG_WAITALL);
	if (n != sizeof(*header))
		return -1;

	if (header->data_len > 0) {
		if (header->data_len > buffer_size)
			return -E2BIG;

		n = recv(sock, data_buffer, header->data_len, MSG_WAITALL);
		if (n != (ssize_t)header->data_len)
			return -1;
	}

	return 0;
}

/* ========== Request processing ========== */

static void process_request(client_info_t *client, btfs_header_t *header,
			    char *data)
{
	int result = 0;
	void *response_data = NULL;
	uint32_t response_len = 0;
	char buffer[BTFS_MAX_DATA];

	client->last_activity = time(NULL);

	switch (header->opcode) {
	case BTFS_OP_GETATTR: {
		btfs_getattr_req_t *req = (btfs_getattr_req_t *)data;
		btfs_attr_t attr;
		result = handle_getattr(req->path, &attr);
		if (!result) {
			response_data = &attr;
			response_len = sizeof(attr);
		}
		break;
	}

	case BTFS_OP_READDIR: {
		btfs_readdir_req_t *req = (btfs_readdir_req_t *)data;
		uint64_t offset;
		result = handle_readdir(req->path, buffer, sizeof(buffer), &offset);
		if (result >= 0) {
			response_data = buffer;
			response_len = offset;
			result = 0;
		}
		break;
	}

	case BTFS_OP_OPEN: {
		btfs_open_req_t *req = (btfs_open_req_t *)data;
		btfs_open_resp_t resp;
		resp.file_handle = handle_open(client->client_id, req->path,
					       req->flags, req->mode, req->lock_type);
		if (resp.file_handle) {
			resp.lock_acquired = (req->lock_type != BTFS_LOCK_NONE);
			response_data = &resp;
			response_len = sizeof(resp);
		} else {
			result = -errno;
		}
		break;
	}

	case BTFS_OP_READ: {
		btfs_read_req_t *req = (btfs_read_req_t *)data;
		uint32_t max_read = BTFS_MAX_DATA - sizeof(btfs_read_resp_t);
		uint32_t read_size = req->size > max_read ? max_read : req->size;
		btfs_read_resp_t *resp = (btfs_read_resp_t *)buffer;
		ssize_t bytes = handle_read(client->client_id, req->file_handle,
					    req->offset, read_size, resp->data);
		if (bytes >= 0) {
			resp->bytes_read = bytes;
			response_data = resp;
			response_len = sizeof(*resp) + bytes;
		} else {
			result = bytes;
		}
		break;
	}

	case BTFS_OP_WRITE: {
		btfs_write_req_t *req = (btfs_write_req_t *)data;
		uint32_t max_write = BTFS_MAX_DATA - sizeof(btfs_write_req_t);
		uint32_t write_size = req->size > max_write ? max_write : req->size;
		btfs_write_resp_t resp;
		ssize_t bytes = handle_write(client->client_id, req->file_handle,
					     req->offset, req->data, write_size);
		if (bytes >= 0) {
			resp.bytes_written = bytes;
			response_data = &resp;
			response_len = sizeof(resp);
		} else {
			result = bytes;
		}
		break;
	}

	case BTFS_OP_CLOSE: {
		btfs_close_req_t *req = (btfs_close_req_t *)data;
		result = handle_close(client->client_id, req->file_handle);
		break;
	}

	case BTFS_OP_FSYNC: {
		btfs_fsync_req_t *req = (btfs_fsync_req_t *)data;
		result = handle_fsync(client->client_id, req->file_handle);
		break;
	}

	case BTFS_OP_MKDIR: {
		btfs_mkdir_req_t *req = (btfs_mkdir_req_t *)data;
		result = handle_mkdir(req->path, req->mode);
		break;
	}

	case BTFS_OP_RMDIR: {
		btfs_unlink_req_t *req = (btfs_unlink_req_t *)data;
		result = handle_rmdir(req->path);
		break;
	}

	case BTFS_OP_CREATE: {
		btfs_create_req_t *req = (btfs_create_req_t *)data;
		result = handle_create(req->path, req->mode, req->flags);
		break;
	}

	case BTFS_OP_UNLINK: {
		btfs_unlink_req_t *req = (btfs_unlink_req_t *)data;
		result = handle_unlink(req->path);
		break;
	}

	case BTFS_OP_RENAME: {
		btfs_rename_req_t *req = (btfs_rename_req_t *)data;
		result = handle_rename(req->oldpath, req->newpath);
		break;
	}

	case BTFS_OP_TRUNCATE: {
		btfs_truncate_req_t *req = (btfs_truncate_req_t *)data;
		result = handle_truncate(req->path, req->size);
		break;
	}

	case BTFS_OP_CHMOD: {
		typedef struct {
			char path[BTFS_MAX_PATH];
			uint32_t mode;
		} __attribute__((packed)) btfs_chmod_req_t;
		btfs_chmod_req_t *req = (btfs_chmod_req_t *)data;
		result = handle_chmod(req->path, req->mode);
		break;
	}

	case BTFS_OP_CHOWN: {
		typedef struct {
			char path[BTFS_MAX_PATH];
			uint32_t uid, gid;
		} __attribute__((packed)) btfs_chown_req_t;
		btfs_chown_req_t *req = (btfs_chown_req_t *)data;
		result = handle_chown(req->path, req->uid, req->gid);
		break;
	}

	case BTFS_OP_SYMLINK: {
		btfs_symlink_req_t *req = (btfs_symlink_req_t *)data;
		result = handle_symlink(req->target, req->linkpath);
		break;
	}

	case BTFS_OP_READLINK: {
		btfs_readlink_req_t *req = (btfs_readlink_req_t *)data;
		btfs_readlink_resp_t resp;
		result = handle_readlink(req->path, resp.target, BTFS_MAX_PATH);
		if (!result) {
			response_data = &resp;
			response_len = sizeof(resp);
		}
		break;
	}

	case BTFS_OP_STATFS: {
		struct statvfs st;
		if (statvfs(base_path, &st) < 0) {
			result = -errno;
		} else {
			struct {
				uint64_t blocks, bfree, bavail, files, ffree;
				uint32_t bsize, namelen;
			} __attribute__((packed)) resp;
			resp.blocks = st.f_blocks;
			resp.bfree = st.f_bfree;
			resp.bavail = st.f_bavail;
			resp.files = st.f_files;
			resp.ffree = st.f_ffree;
			resp.bsize = st.f_bsize;
			resp.namelen = NAME_MAX;
			response_data = &resp;
			response_len = sizeof(resp);
		}
		break;
	}

	case BTFS_OP_LOCK: {
		btfs_lock_req_t *req = (btfs_lock_req_t *)data;
		btfs_lock_resp_t resp;
		result = check_lock_conflict(req->path, req->lock_type, client->client_id);
		if (!result) {
			resp.lock_id = acquire_lock(req->path, req->lock_type,
						    client->client_id, req->timeout_sec);
			if (resp.lock_id) {
				response_data = &resp;
				response_len = sizeof(resp);
			} else {
				result = -ENOMEM;
			}
		}
		break;
	}

	case BTFS_OP_UNLOCK: {
		btfs_lock_req_t *req = (btfs_lock_req_t *)data;
		uint32_t lock_id;
		pthread_mutex_lock(&locks_mutex);
		file_lock_t *lock = find_lock(req->path, client->client_id);
		lock_id = lock ? lock->lock_id : 0;
		pthread_mutex_unlock(&locks_mutex);
		result = lock_id ? release_lock(lock_id, client->client_id) : -ENOENT;
		break;
	}

	case BTFS_OP_PING:
		result = 0;
		break;

	default:
		result = -ENOSYS;
		break;
	}

	send_response(client->socket, header->sequence, header->opcode,
		      result, response_data, response_len);
}

/* ========== Client thread ========== */

static void *client_thread(void *arg)
{
	client_info_t *client = arg;
	btfs_header_t header;
	char data_buffer[BTFS_MAX_DATA];
	struct timeval timeout = { .tv_sec = 1 };

	setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	while (server_running && client->active) {
		int ret = receive_request(client->socket, &header,
					  data_buffer, sizeof(data_buffer));
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (time(NULL) - client->last_activity > CLIENT_TIMEOUT)
					break;
				continue;
			}
			break;
		}

		process_request(client, &header, data_buffer);
	}

	close_client_files(client->client_id);
	release_client_locks(client->client_id);
	close(client->socket);
	client->active = 0;

	return NULL;
}

/* ========== SDP registration ========== */

static sdp_session_t *register_sdp_service(uint8_t channel)
{
	uint32_t service_uuid_int[] = { 0x01110000, 0x00100000, 0x80000080, 0xFB349B5F };
	uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid, svc_class_uuid;
	sdp_list_t *l2cap_list = NULL, *rfcomm_list = NULL, *root_list = NULL,
		   *proto_list = NULL, *access_proto_list = NULL,
		   *svc_class_list = NULL, *profile_list = NULL;
	sdp_data_t *channel_data;
	sdp_profile_desc_t profile;
	sdp_record_t *record = sdp_record_alloc();
	sdp_session_t *session;

	sdp_uuid128_create(&svc_uuid, &service_uuid_int);
	sdp_set_service_id(record, svc_uuid);

	sdp_uuid32_create(&svc_class_uuid, SERIAL_PORT_SVCLASS_ID);
	svc_class_list = sdp_list_append(NULL, &svc_class_uuid);
	sdp_set_service_classes(record, svc_class_list);

	sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
	profile.version = 0x0100;
	profile_list = sdp_list_append(NULL, &profile);
	sdp_set_profile_descs(record, profile_list);

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root_list = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root_list);

	sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
	l2cap_list = sdp_list_append(NULL, &l2cap_uuid);
	proto_list = sdp_list_append(NULL, l2cap_list);

	sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
	channel_data = sdp_data_alloc(SDP_UINT8, &channel);
	rfcomm_list = sdp_list_append(NULL, &rfcomm_uuid);
	sdp_list_append(rfcomm_list, channel_data);
	sdp_list_append(proto_list, rfcomm_list);

	access_proto_list = sdp_list_append(NULL, proto_list);
	sdp_set_access_protos(record, access_proto_list);

	sdp_set_info_attr(record, "BTFS File System Server", "BTFS",
			  "Bluetooth Distributed File System");

	session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
	if (!session)
		return NULL;

	if (sdp_record_register(session, record, 0) < 0) {
		sdp_close(session);
		return NULL;
	}

	sdp_data_free(channel_data);
	sdp_list_free(l2cap_list, 0);
	sdp_list_free(rfcomm_list, 0);
	sdp_list_free(root_list, 0);
	sdp_list_free(access_proto_list, 0);
	sdp_list_free(svc_class_list, 0);
	sdp_list_free(profile_list, 0);

	return session;
}

/* ========== Signal handling ========== */

static void signal_handler(int sig)
{
	if (sig == SIGINT || sig == SIGTERM)
		server_running = 0;
}

/* ========== Main ========== */

int main(int argc, char **argv)
{
	struct sockaddr_rc loc_addr = {
		.rc_family = AF_BLUETOOTH,
		.rc_bdaddr = *BDADDR_ANY,
		.rc_channel = 1,
	};
	struct sockaddr_rc rem_addr;
	socklen_t opt;
	struct timeval tv = { .tv_sec = 1 };
	int server_sock, client_sock, slot;
	sdp_session_t *sdp;
	pthread_t lock_thread;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <SHARED_DIRECTORY>\n", argv[0]);
		return 1;
	}

	base_path = realpath(argv[1], NULL);
	if (!base_path) {
		perror("Invalid directory");
		return 1;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	log_msg("===================================================");
	log_msg(" BTFS SERVER - Bluetooth File System");
	log_msg("===================================================");
	log_msg("Shared directory: %s", base_path);
	log_msg("Max clients: %d", MAX_CLIENTS);

	memset(clients, 0, sizeof(clients));
	memset(open_files, 0, sizeof(open_files));

	pthread_create(&lock_thread, NULL, lock_timeout_thread, NULL);
	pthread_detach(lock_thread);

	server_sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (server_sock < 0 ||
	    bind(server_sock, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
		perror("Socket/bind failed");
		goto cleanup;
	}

	sdp = register_sdp_service(1);
	if (!sdp)
		log_msg("WARNING: SDP registration failed");

	if (listen(server_sock, MAX_CLIENTS) < 0) {
		perror("Listen failed");
		goto cleanup;
	}

	log_msg("Server listening on RFCOMM channel 1");
	log_msg("===================================================\n");

	while (server_running) {
		setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		opt = sizeof(rem_addr);
		client_sock = accept(server_sock, (struct sockaddr *)&rem_addr, &opt);

		if (client_sock < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			if (server_running)
				perror("Accept failed");
			continue;
		}

		pthread_mutex_lock(&clients_mutex);
		slot = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (!clients[i].active) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			pthread_mutex_unlock(&clients_mutex);
			close(client_sock);
			continue;
		}

		clients[slot].socket = client_sock;
		clients[slot].client_id = next_client_id++;
		clients[slot].addr = rem_addr;
		clients[slot].connect_time = time(NULL);
		clients[slot].last_activity = time(NULL);
		clients[slot].active = 1;

		if (pthread_create(&clients[slot].thread, NULL,
				   client_thread, &clients[slot]) != 0) {
			close(client_sock);
			clients[slot].active = 0;
		} else {
			pthread_detach(clients[slot].thread);
		}

		pthread_mutex_unlock(&clients_mutex);
	}

cleanup:
	log_msg("\n===================================================");
	log_msg("Shutting down server...");

	if (server_sock >= 0)
		close(server_sock);
	if (sdp)
		sdp_close(sdp);

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].active) {
			close(clients[i].socket);
			clients[i].active = 0;
		}
	}

	sleep(2);
	free(base_path);

	log_msg("Server stopped");
	log_msg("===================================================");

	return 0;
}

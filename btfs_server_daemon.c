#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <signal.h>
#include <stdarg.h>

#include "btfs_protocol.h"

#define MAX_CLIENTS 10
#define MAX_LOCKS 1000
#define MAX_OPEN_FILES 1000
#define LOCK_CHECK_INTERVAL 5

// ============ СТРУКТУРЫ ДАННЫХ ============

typedef struct file_lock {
    uint32_t lock_id;
    char path[BTFS_MAX_PATH];
    uint32_t client_id;
    btfs_lock_type_t type;
    time_t acquired_time;
    time_t timeout_time;
    int ref_count;
    struct file_lock *next;
} file_lock_t;

typedef struct open_file {
    uint64_t handle;
    int fd;
    char path[BTFS_MAX_PATH];
    uint32_t client_id;
    uint32_t flags;
    uint32_t lock_id;
    time_t last_access;
} open_file_t;

typedef struct client_info {
    int socket;
    uint32_t client_id;
    struct sockaddr_rc addr;
    pthread_t thread;
    time_t connect_time;
    time_t last_activity;
    int active;
} client_info_t;

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============

static char *base_path = NULL;
static client_info_t clients[MAX_CLIENTS];
static open_file_t open_files[MAX_OPEN_FILES];
static file_lock_t *locks_head = NULL;
static pthread_mutex_t locks_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_client_id = 1;
static uint64_t next_file_handle = 1;
static uint32_t next_lock_id = 1;
static volatile int server_running = 1;

// ============ ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ============

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

int build_full_path(char *full_path, size_t size, const char *rel_path) {
    return snprintf(full_path, size, "%s/%s", base_path, rel_path) >= (int)size 
           ? -ENAMETOOLONG : 0;
}

// Найти открытый файл по handle и client_id
static open_file_t* find_open_file(uint64_t handle, uint32_t client_id) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].handle == handle && open_files[i].client_id == client_id) {
            open_files[i].last_access = time(NULL);
            return &open_files[i];
        }
    }
    return NULL;
}

// Найти блокировку по пути и клиенту
static file_lock_t* find_lock(const char *path, uint32_t client_id) {
    file_lock_t *lock = locks_head;
    while (lock) {
        if (strcmp(lock->path, path) == 0 && lock->client_id == client_id) {
            return lock;
        }
        lock = lock->next;
    }
    return NULL;
}

// ============ МЕНЕДЖЕР БЛОКИРОВОК ============

int check_lock_conflict(const char *path, btfs_lock_type_t new_type, uint32_t client_id) {
    pthread_mutex_lock(&locks_mutex);
    
    file_lock_t *lock = locks_head;
    while (lock) {
        if (strcmp(lock->path, path) == 0) {
            // Read-Read OK, тот же клиент OK
            if ((new_type == BTFS_LOCK_READ && lock->type == BTFS_LOCK_READ) || 
                (lock->client_id == client_id)) {
                pthread_mutex_unlock(&locks_mutex);
                return 0;
            }
            pthread_mutex_unlock(&locks_mutex);
            return -EAGAIN;
        }
        lock = lock->next;
    }
    
    pthread_mutex_unlock(&locks_mutex);
    return 0;
}

uint32_t acquire_lock(const char *path, btfs_lock_type_t type,
                      uint32_t client_id, uint32_t timeout_sec) {
    pthread_mutex_lock(&locks_mutex);
    
    // Обновить существующую
    file_lock_t *lock = find_lock(path, client_id);
    if (lock) {
        lock->type = type;
        lock->acquired_time = time(NULL);
        lock->timeout_time = timeout_sec > 0 ? lock->acquired_time + timeout_sec : 0;
        uint32_t id = lock->lock_id;
        pthread_mutex_unlock(&locks_mutex);
        return id;
    }
    
    // Создать новую
    lock = malloc(sizeof(file_lock_t));
    if (!lock) {
        pthread_mutex_unlock(&locks_mutex);
        return 0;
    }
    
    lock->lock_id = next_lock_id++;
    strncpy(lock->path, path, BTFS_MAX_PATH - 1);
    lock->client_id = client_id;
    lock->type = type;
    lock->acquired_time = time(NULL);
    lock->timeout_time = timeout_sec > 0 ? lock->acquired_time + timeout_sec : 0;
    lock->ref_count = 1;
    lock->next = locks_head;
    locks_head = lock;
    
    uint32_t id = lock->lock_id;
    pthread_mutex_unlock(&locks_mutex);
    
    print_log("[LockMgr] Lock acquired: id=%u, client=%u, path=%s, type=%s",
              id, client_id, path, type == BTFS_LOCK_READ ? "READ" : "WRITE");
    
    return id;
}

int release_lock(uint32_t lock_id, uint32_t client_id) {
    pthread_mutex_lock(&locks_mutex);
    
    file_lock_t *lock = locks_head, *prev = NULL;
    while (lock) {
        if (lock->lock_id == lock_id && lock->client_id == client_id) {
            if (prev) prev->next = lock->next;
            else locks_head = lock->next;
            
            print_log("[LockMgr] Lock released: id=%u, client=%u, path=%s",
                      lock_id, client_id, lock->path);
            
            free(lock);
            pthread_mutex_unlock(&locks_mutex);
            return 0;
        }
        prev = lock;
        lock = lock->next;
    }
    
    pthread_mutex_unlock(&locks_mutex);
    return -ENOENT;
}

void release_client_locks(uint32_t client_id) {
    pthread_mutex_lock(&locks_mutex);
    
    file_lock_t *lock = locks_head, *prev = NULL;
    while (lock) {
        if (lock->client_id == client_id) {
            file_lock_t *to_free = lock;
            print_log("[LockMgr] Auto-releasing lock: id=%u, client=%u, path=%s",
                      lock->lock_id, client_id, lock->path);
            
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

void *lock_timeout_thread(void *arg) {
    while (server_running) {
        sleep(LOCK_CHECK_INTERVAL);
        
        time_t now = time(NULL);
        pthread_mutex_lock(&locks_mutex);
        
        file_lock_t *lock = locks_head, *prev = NULL;
        while (lock) {
            if (lock->timeout_time > 0 && now >= lock->timeout_time) {
                print_log("[LockMgr] Lock timeout: id=%u, client=%u, path=%s",
                          lock->lock_id, lock->client_id, lock->path);
                
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

// ============ ФАЙЛОВЫЕ ОПЕРАЦИИ ============

uint64_t handle_open(uint32_t client_id, const char *rel_path,
                     uint32_t flags, uint32_t mode, uint32_t lock_type) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) {
        errno = ENAMETOOLONG;
        return 0;
    }
    
    // Проверить блокировки
    if (lock_type != BTFS_LOCK_NONE) {
        if (check_lock_conflict(rel_path, lock_type, client_id) < 0) {
            print_log("[Client %u] Open blocked due to lock conflict: %s", client_id, rel_path);
            errno = EAGAIN;
            return 0;
        }
    }
    
    // ИСПРАВЛЕНО: Правильная конвертация флагов из BTFS в POSIX
    int posix_flags = 0;
    
    // Режим доступа (взаимоисключающие)
    if ((flags & O_ACCMODE) == O_WRONLY) {
        posix_flags = O_WRONLY;
    } else if ((flags & O_ACCMODE) == O_RDWR) {
        posix_flags = O_RDWR;
    } else {
        posix_flags = O_RDONLY;
    }
    
    // Дополнительные флаги
    if (flags & O_APPEND) posix_flags |= O_APPEND;
    if (flags & O_TRUNC)  posix_flags |= O_TRUNC;
    if (flags & O_CREAT)  posix_flags |= O_CREAT;
    if (flags & O_EXCL)   posix_flags |= O_EXCL;
    if (flags & O_SYNC)   posix_flags |= O_SYNC;
    
    print_log("[Client %u] Opening: %s flags=0x%x->0x%x mode=0%o",
              client_id, rel_path, flags, posix_flags, mode);
    
    int fd = open(full_path, posix_flags, mode ? mode : 0644);
    if (fd < 0) {
        int err = errno;
        print_log("[Client %u] Open failed: %s -> %s (errno=%d)",
                  client_id, rel_path, strerror(err), err);
        errno = err;
        return 0;
    }
    
    uint32_t lock_id = 0;
    if (lock_type != BTFS_LOCK_NONE) {
        lock_id = acquire_lock(rel_path, lock_type, client_id, 0);
        if (!lock_id) {
            print_log("[Client %u] Failed to acquire lock for: %s", client_id, rel_path);
            close(fd);
            errno = ENOMEM;
            return 0;
        }
    }
    
    pthread_mutex_lock(&files_mutex);
    
    int slot = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].handle == 0) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        pthread_mutex_unlock(&files_mutex);
        close(fd);
        if (lock_id) release_lock(lock_id, client_id);
        print_log("[Client %u] No free slots in open_files table", client_id);
        errno = ENFILE;
        return 0;
    }
    
    open_files[slot].handle = next_file_handle++;
    open_files[slot].fd = fd;
    strncpy(open_files[slot].path, rel_path, BTFS_MAX_PATH - 1);
    open_files[slot].path[BTFS_MAX_PATH - 1] = '\0';
    open_files[slot].client_id = client_id;
    open_files[slot].flags = posix_flags;  // ИСПРАВЛЕНО: сохраняем POSIX флаги
    open_files[slot].lock_id = lock_id;
    open_files[slot].last_access = time(NULL);
    
    uint64_t handle = open_files[slot].handle;
    pthread_mutex_unlock(&files_mutex);
    
    print_log("[Client %u] File opened: %s (handle=%lu, fd=%d, posix_flags=0x%x, lock=%u)",
              client_id, rel_path, handle, fd, posix_flags, lock_id);
    
    return handle;
}


int handle_close(uint32_t client_id, uint64_t file_handle) {
    pthread_mutex_lock(&files_mutex);
    
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].handle == file_handle && open_files[i].client_id == client_id) {
            close(open_files[i].fd);
            
            if (open_files[i].lock_id) {
                release_lock(open_files[i].lock_id, client_id);
            }
            
            print_log("[Client %u] File closed: %s (handle=%lu)",
                      client_id, open_files[i].path, file_handle);
            
            memset(&open_files[i], 0, sizeof(open_file_t));
            pthread_mutex_unlock(&files_mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&files_mutex);
    return -EBADF;
}

void close_client_files(uint32_t client_id) {
    pthread_mutex_lock(&files_mutex);
    
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].handle != 0 && open_files[i].client_id == client_id) {
            close(open_files[i].fd);
            if (open_files[i].lock_id) release_lock(open_files[i].lock_id, client_id);
            
            print_log("[Client %u] Auto-closing file: %s (handle=%lu)",
                      client_id, open_files[i].path, open_files[i].handle);
            
            memset(&open_files[i], 0, sizeof(open_file_t));
        }
    }
    
    pthread_mutex_unlock(&files_mutex);
}

ssize_t handle_read(uint32_t client_id, uint64_t file_handle,
                    uint64_t offset, uint32_t size, char *buffer) {
    pthread_mutex_lock(&files_mutex);
    open_file_t *file = find_open_file(file_handle, client_id);
    int fd = file ? file->fd : -1;
    pthread_mutex_unlock(&files_mutex);
    
    if (fd < 0) return -EBADF;
    
    ssize_t ret = pread(fd, buffer, size, offset);
    return ret < 0 ? -errno : ret;
}

ssize_t handle_write(uint32_t client_id, uint64_t file_handle,
                     uint64_t offset, const char *data, uint32_t size) {
    pthread_mutex_lock(&files_mutex);
    open_file_t *file = find_open_file(file_handle, client_id);
    int fd = file ? file->fd : -1;
    pthread_mutex_unlock(&files_mutex);
    
    if (fd < 0) return -EBADF;
    
    ssize_t ret = pwrite(fd, data, size, offset);
    return ret < 0 ? -errno : ret;
}

int handle_getattr(const char *rel_path, btfs_attr_t *attr) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    
    struct stat st;
    if (lstat(full_path, &st) < 0) return -errno;
    
    memset(attr, 0, sizeof(btfs_attr_t));
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

int handle_readdir(const char *rel_path, char *buffer, size_t buffer_size, uint64_t *offset_out) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    
    DIR *dir = opendir(full_path);
    if (!dir) return -errno;
    
    size_t used = 0;
    int count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        
        size_t name_len = strlen(entry->d_name);
        size_t entry_size = sizeof(btfs_dirent_t) + name_len + 1;
        
        if (used + entry_size > buffer_size) break;
        
        btfs_dirent_t *dirent = (btfs_dirent_t*)(buffer + used);
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

int handle_rmdir(const char *rel_path) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    return rmdir(full_path) < 0 ? -errno : 0;
}

int handle_unlink(const char *rel_path) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    return unlink(full_path) < 0 ? -errno : 0;
}

int handle_mkdir(const char *rel_path, uint32_t mode) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    return mkdir(full_path, mode) < 0 ? -errno : 0;
}

int handle_create(const char *rel_path, uint32_t mode, uint32_t flags) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    
    int fd = open(full_path, O_CREAT | O_EXCL | O_WRONLY | flags, mode);
    if (fd < 0) return -errno;
    
    close(fd);
    return 0;
}

int handle_rename(const char *oldpath, const char *newpath) {
    char old_full[PATH_MAX], new_full[PATH_MAX];
    if (build_full_path(old_full, sizeof(old_full), oldpath) < 0 ||
        build_full_path(new_full, sizeof(new_full), newpath) < 0) {
        return -ENAMETOOLONG;
    }
    return rename(old_full, new_full) < 0 ? -errno : 0;
}

int handle_truncate(const char *rel_path, uint64_t size) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    return truncate(full_path, size) < 0 ? -errno : 0;
}

int handle_chmod(const char *rel_path, uint32_t mode) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    return chmod(full_path, mode) < 0 ? -errno : 0;
}

int handle_chown(const char *rel_path, uint32_t uid, uint32_t gid) {
    char full_path[PATH_MAX];
    if (build_full_path(full_path, sizeof(full_path), rel_path) < 0) return -ENAMETOOLONG;
    return chown(full_path, uid, gid) < 0 ? -errno : 0;
}

// ============ ПРОТОКОЛ ============

int send_response(int sock, uint32_t sequence, uint32_t opcode,
                  int32_t result, const void *data, uint32_t data_len) {
    btfs_response_t resp = { .opcode = opcode, .sequence = sequence, 
                             .result = result, .data_len = data_len };
    
    if (write(sock, &resp, sizeof(resp)) != sizeof(resp)) return -1;
    if (data && data_len > 0 && write(sock, data, data_len) != data_len) return -1;
    
    return 0;
}

int receive_request(int sock, btfs_header_t *header, char *data_buffer, size_t buffer_size) {
    if (read(sock, header, sizeof(btfs_header_t)) != sizeof(btfs_header_t)) return -1;
    
    if (header->data_len > 0) {
        if (header->data_len > buffer_size) return -E2BIG;
        if (read(sock, data_buffer, header->data_len) != header->data_len) return -1;
    }
    
    return 0;
}

// ============ ОБРАБОТЧИК ЗАПРОСОВ ============

void process_request(client_info_t *client, btfs_header_t *header, char *data) {
    int result = 0;
    void *response_data = NULL;
    uint32_t response_len = 0;
    char buffer[BTFS_MAX_DATA];
    client->last_activity = time(NULL);

    switch (header->opcode) {
        case BTFS_OP_GETATTR: {
            btfs_getattr_req_t *req = (btfs_getattr_req_t*)data;
            btfs_attr_t attr;
            result = handle_getattr(req->path, &attr);
            if (result == 0) {
                response_data = &attr;
                response_len = sizeof(attr);
            }
            print_log("[Client %u] GETATTR: %s -> %d", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_READDIR: {
            btfs_readdir_req_t *req = (btfs_readdir_req_t*)data;
            uint64_t offset;
            result = handle_readdir(req->path, buffer, sizeof(buffer), &offset);
            if (result >= 0) {
                response_data = buffer;
                response_len = offset;
                result = 0;
            }
            print_log("[Client %u] READDIR: %s -> %d entries", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_OPEN: {
            btfs_open_req_t *req = (btfs_open_req_t*)data;
            btfs_open_resp_t resp;
            resp.file_handle = handle_open(client->client_id, req->path, req->flags, req->mode, req->lock_type);
            if (resp.file_handle > 0) {
                resp.lock_acquired = (req->lock_type != BTFS_LOCK_NONE);
                response_data = &resp;
                response_len = sizeof(resp);
            } else {
                result = -errno;
            }
            print_log("[Client %u] OPEN: %s -> handle=%lu, result=%d",
                      client->client_id, req->path, resp.file_handle, result);
            break;
        }
        
        case BTFS_OP_READ: {
            btfs_read_req_t *req = (btfs_read_req_t*)data;
            
            // ИСПРАВЛЕНИЕ: ограничить размер чтения
            uint32_t max_read = BTFS_MAX_DATA - sizeof(btfs_read_resp_t);
            uint32_t read_size = req->size > max_read ? max_read : req->size;
            
            btfs_read_resp_t *resp = (btfs_read_resp_t*)buffer;
            ssize_t bytes = handle_read(client->client_id, req->file_handle, 
                                       req->offset, read_size, resp->data);
            
            if (bytes >= 0) {
                resp->bytes_read = bytes;
                response_data = resp;
                response_len = sizeof(btfs_read_resp_t) + bytes;
            } else {
                result = bytes;
            }
            
            print_log("[Client %u] READ: handle=%lu, offset=%lu, requested=%u, read=%zd", 
                     client->client_id, req->file_handle, req->offset, req->size, bytes);
            break;
        }
        
        case BTFS_OP_WRITE: {
            btfs_write_req_t *req = (btfs_write_req_t*)data;
            btfs_write_resp_t resp;
            
            // ИСПРАВЛЕНИЕ: ограничить размер записи
            uint32_t max_write = BTFS_MAX_DATA - sizeof(btfs_write_req_t);
            uint32_t write_size = req->size > max_write ? max_write : req->size;
            
            ssize_t bytes = handle_write(client->client_id, req->file_handle, 
                                        req->offset, req->data, write_size);
            
            if (bytes >= 0) {
                resp.bytes_written = bytes;
                response_data = &resp;
                response_len = sizeof(resp);
            } else {
                result = bytes;
            }
            
            print_log("[Client %u] WRITE: handle=%lu, offset=%lu, requested=%u, written=%zd", 
                     client->client_id, req->file_handle, req->offset, req->size, bytes);
            break;
        }
        
        case BTFS_OP_CLOSE: {
            btfs_close_req_t *req = (btfs_close_req_t*)data;
            result = handle_close(client->client_id, req->file_handle);
            print_log("[Client %u] CLOSE: handle=%lu -> %d", client->client_id, req->file_handle, result);
            break;
        }
        
        case BTFS_OP_MKDIR: {
            btfs_mkdir_req_t *req = (btfs_mkdir_req_t*)data;
            result = handle_mkdir(req->path, req->mode);
            print_log("[Client %u] MKDIR: %s -> %d", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_RMDIR: {
            btfs_unlink_req_t *req = (btfs_unlink_req_t*)data;
            result = handle_rmdir(req->path);
            print_log("[Client %u] RMDIR: %s -> %d", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_CREATE: {
            btfs_create_req_t *req = (btfs_create_req_t*)data;
            result = handle_create(req->path, req->mode, req->flags);
            print_log("[Client %u] CREATE: %s -> %d", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_UNLINK: {
            btfs_unlink_req_t *req = (btfs_unlink_req_t*)data;
            result = handle_unlink(req->path);
            print_log("[Client %u] UNLINK: %s -> %d", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_RENAME: {
            btfs_rename_req_t *req = (btfs_rename_req_t*)data;
            result = handle_rename(req->oldpath, req->newpath);
            print_log("[Client %u] RENAME: %s -> %s : %d", client->client_id, req->oldpath, req->newpath, result);
            break;
        }
        
        case BTFS_OP_TRUNCATE: {
            btfs_truncate_req_t *req = (btfs_truncate_req_t*)data;
            result = handle_truncate(req->path, req->size);
            print_log("[Client %u] TRUNCATE: %s -> size=%lu, result=%d", 
                     client->client_id, req->path, req->size, result);
            break;
        }
        
        case BTFS_OP_LOCK: {
            btfs_lock_req_t *req = (btfs_lock_req_t*)data;
            btfs_lock_resp_t resp;
            result = check_lock_conflict(req->path, req->lock_type, client->client_id);
            if (result == 0) {
                resp.lock_id = acquire_lock(req->path, req->lock_type, client->client_id, req->timeout_sec);
                if (resp.lock_id > 0) {
                    response_data = &resp;
                    response_len = sizeof(resp);
                } else {
                    result = -ENOMEM;
                }
            }
            print_log("[Client %u] LOCK: %s -> lock_id=%u, result=%d",
                      client->client_id, req->path, resp.lock_id, result);
            break;
        }
        
        case BTFS_OP_UNLOCK: {
            btfs_lock_req_t *req = (btfs_lock_req_t*)data;
            pthread_mutex_lock(&locks_mutex);
            file_lock_t *lock = find_lock(req->path, client->client_id);
            uint32_t lock_id = lock ? lock->lock_id : 0;
            pthread_mutex_unlock(&locks_mutex);
            result = lock_id > 0 ? release_lock(lock_id, client->client_id) : -ENOENT;
            print_log("[Client %u] UNLOCK: %s -> %d", client->client_id, req->path, result);
            break;
        }
        
        case BTFS_OP_PING:
            result = 0;
            print_log("[Client %u] PING", client->client_id);
            break;
            
        default:
            result = -ENOSYS;
            print_log("[Client %u] UNKNOWN OPCODE: %u", client->client_id, header->opcode);
            break;
    }
    
    send_response(client->socket, header->sequence, header->opcode, result, response_data, response_len);
}


// ============ КЛИЕНТСКИЙ ПОТОК ============

void *client_thread(void *arg) {
    client_info_t *client = (client_info_t*)arg;
    btfs_header_t header;
    char data_buffer[BTFS_MAX_DATA];
    char addr_str[18];
    
    ba2str(&client->addr.rc_bdaddr, addr_str);
    print_log("[Client %u] Thread started: %s", client->client_id, addr_str);
    
    // Установить таймаут для read
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    while (server_running && client->active) {
        int ret = receive_request(client->socket, &header, data_buffer, sizeof(data_buffer));
        
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - проверить неактивность
                if (time(NULL) - client->last_activity > 60) {
                    print_log("[Client %u] Timeout - closing connection", client->client_id);
                    break;
                }
                continue;
            }
            
            print_log("[Client %u] Read error: %s", client->client_id, strerror(errno));
            break;
        }
        
        process_request(client, &header, data_buffer);
    }
    
    print_log("[Client %u] Disconnecting: %s", client->client_id, addr_str);
    close_client_files(client->client_id);
    release_client_locks(client->client_id);
    close(client->socket);
    client->active = 0;
    
    print_log("[Client %u] Thread finished", client->client_id);
    return NULL;
}


// ============ SDP ============

sdp_session_t *register_sdp_service(uint8_t channel) {
    uint32_t service_uuid_int[] = {0x01110000, 0x00100000, 0x80000080, 0xFB349B5F};
    uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid, svc_class_uuid;
    sdp_list_t *l2cap_list = 0, *rfcomm_list = 0, *root_list = 0,
               *proto_list = 0, *access_proto_list = 0, *svc_class_list = 0, *profile_list = 0;
    sdp_data_t *channel_data = 0;
    sdp_profile_desc_t profile;
    sdp_record_t *record = sdp_record_alloc();
    sdp_session_t *session = 0;
    
    sdp_uuid128_create(&svc_uuid, &service_uuid_int);
    sdp_set_service_id(record, svc_uuid);
    
    sdp_uuid32_create(&svc_class_uuid, SERIAL_PORT_SVCLASS_ID);
    svc_class_list = sdp_list_append(0, &svc_class_uuid);
    sdp_set_service_classes(record, svc_class_list);
    
    sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
    profile.version = 0x0100;
    profile_list = sdp_list_append(0, &profile);
    sdp_set_profile_descs(record, profile_list);
    
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root_list = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups(record, root_list);
    
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    l2cap_list = sdp_list_append(0, &l2cap_uuid);
    proto_list = sdp_list_append(0, l2cap_list);
    
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    channel_data = sdp_data_alloc(SDP_UINT8, &channel);
    rfcomm_list = sdp_list_append(0, &rfcomm_uuid);
    sdp_list_append(rfcomm_list, channel_data);
    sdp_list_append(proto_list, rfcomm_list);
    
    access_proto_list = sdp_list_append(0, proto_list);
    sdp_set_access_protos(record, access_proto_list);
    
    sdp_set_info_attr(record, "BTFS File System Server", "BTFS", "Bluetooth Distributed File System");
    
    session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
    if (!session) return NULL;
    
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

// ============ MAIN ============

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        print_log("Received shutdown signal");
        server_running = 0;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <shared_directory>\n", argv[0]);
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
    
    print_log("===================================================");
    print_log("  BTFS SERVER - Bluetooth File System");
    print_log("===================================================");
    print_log("Shared directory: %s", base_path);
    print_log("Max clients: %d", MAX_CLIENTS);
    
    memset(clients, 0, sizeof(clients));
    memset(open_files, 0, sizeof(open_files));
    
    pthread_t lock_thread;
    pthread_create(&lock_thread, NULL, lock_timeout_thread, NULL);
    pthread_detach(lock_thread);
    
    int server_sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (server_sock < 0) {
        perror("Socket creation failed");
        free(base_path);
        return 1;
    }
    
    struct sockaddr_rc loc_addr = { .rc_family = AF_BLUETOOTH, .rc_bdaddr = *BDADDR_ANY, .rc_channel = 1 };
    
    if (bind(server_sock, (struct sockaddr*)&loc_addr, sizeof(loc_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        free(base_path);
        return 1;
    }
    
    sdp_session_t *sdp = register_sdp_service(1);
    if (!sdp) print_log("WARNING: SDP registration failed");
    else print_log("SDP service registered on channel 1");
    
    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        if (sdp) sdp_close(sdp);
        close(server_sock);
        free(base_path);
        return 1;
    }
    
    print_log("Server listening on RFCOMM channel 1");
    print_log("===================================================\n");
    
     while (server_running) {
        struct sockaddr_rc rem_addr = {0};
        socklen_t opt = sizeof(rem_addr);
        
        // Установить таймаут для accept
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        int client_sock = accept(server_sock, (struct sockaddr*)&rem_addr, &opt);
        
        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Timeout - проверить server_running
            }
            if (server_running) perror("Accept failed");
            continue;
        }
        
        pthread_mutex_lock(&clients_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                slot = i;
                break;
            }
        }
        
        if (slot < 0) {
            pthread_mutex_unlock(&clients_mutex);
            print_log("WARNING: Max clients reached - rejecting connection");
            close(client_sock);
            continue;
        }
        
        clients[slot].socket = client_sock;
        clients[slot].client_id = next_client_id++;
        clients[slot].addr = rem_addr;
        clients[slot].connect_time = time(NULL);
        clients[slot].last_activity = time(NULL);
        clients[slot].active = 1;
        
        char addr_str[18];
        ba2str(&rem_addr.rc_bdaddr, addr_str);
        print_log("New client connected: %s (ID=%u)", addr_str, clients[slot].client_id);
        
        if (pthread_create(&clients[slot].thread, NULL, client_thread, &clients[slot]) != 0) {
            perror("Thread creation failed");
            close(client_sock);
            clients[slot].active = 0;
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
        
        pthread_detach(clients[slot].thread);
        pthread_mutex_unlock(&clients_mutex);
    }
    
    print_log("\n===================================================");
    print_log("Shutting down server...");
    
    close(server_sock);
    if (sdp) sdp_close(sdp);
    
    // Закрыть все клиентские соединения
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            close(clients[i].socket);
            clients[i].active = 0;
        }
    }
    
    // Подождать завершения потоков (максимум 3 секунды)
    sleep(2);
    
    free(base_path);
    print_log("Server stopped");
    print_log("===================================================");
    
    return 0;
}

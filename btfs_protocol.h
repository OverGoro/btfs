#ifndef BTFS_PROTOCOL_H
#define BTFS_PROTOCOL_H

#include <stdint.h>
#include <sys/stat.h>
#include <linux/limits.h>

#define BTFS_MAX_PATH 256
#define BTFS_MAX_DATA 4096

// Opcodes
typedef enum {
    BTFS_OP_GETATTR = 1,
    BTFS_OP_READDIR = 2,
    BTFS_OP_ACCESS = 3,
    BTFS_OP_STATFS = 4,
    BTFS_OP_OPEN = 10,
    BTFS_OP_READ = 11,
    BTFS_OP_WRITE = 12,
    BTFS_OP_CLOSE = 13,
    BTFS_OP_FSYNC = 14,
    BTFS_OP_MKDIR = 20,
    BTFS_OP_RMDIR = 21,
    BTFS_OP_CREATE = 30,
    BTFS_OP_UNLINK = 31,
    BTFS_OP_RENAME = 32,
    BTFS_OP_TRUNCATE = 33,
    BTFS_OP_CHMOD = 34,
    BTFS_OP_CHOWN = 35,
    BTFS_OP_UTIMENS = 36,
    BTFS_OP_LOCK = 40,
    BTFS_OP_UNLOCK = 41,
    BTFS_OP_PING = 50,
    BTFS_OP_DISCONNECT = 51
} btfs_opcode_t;

// Lock types
typedef enum {
    BTFS_LOCK_NONE = 0,
    BTFS_LOCK_READ = 1,
    BTFS_LOCK_WRITE = 2
} btfs_lock_type_t;

// Request header
typedef struct {
    uint32_t opcode;
    uint32_t sequence;
    uint32_t client_id;
    uint32_t flags;
    uint32_t data_len;
} __attribute__((packed)) btfs_header_t;

// Response header
typedef struct {
    uint32_t opcode;
    uint32_t sequence;
    int32_t result;
    uint32_t data_len;
} __attribute__((packed)) btfs_response_t;

// Request structures
typedef struct {
    char path[BTFS_MAX_PATH];
} __attribute__((packed)) btfs_getattr_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint64_t offset;
} __attribute__((packed)) btfs_readdir_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t flags;
    uint32_t mode;
    uint32_t lock_type;
} __attribute__((packed)) btfs_open_req_t;

typedef struct {
    uint64_t file_handle;
    uint64_t offset;
    uint32_t size;
} __attribute__((packed)) btfs_read_req_t;

typedef struct {
    uint64_t file_handle;
    uint64_t offset;
    uint32_t size;
    char data[0];
} __attribute__((packed)) btfs_write_req_t;

typedef struct {
    uint64_t file_handle;
} __attribute__((packed)) btfs_close_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t mode;
} __attribute__((packed)) btfs_mkdir_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t mode;
    uint32_t flags;
} __attribute__((packed)) btfs_create_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
} __attribute__((packed)) btfs_unlink_req_t;

typedef struct {
    char oldpath[BTFS_MAX_PATH];
    char newpath[BTFS_MAX_PATH];
} __attribute__((packed)) btfs_rename_req_t;

// ДОБАВИТЬ ЭТУ СТРУКТУРУ:
typedef struct {
    char path[BTFS_MAX_PATH];
    uint64_t size;
} __attribute__((packed)) btfs_truncate_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t lock_type;
    uint32_t timeout_sec;
} __attribute__((packed)) btfs_lock_req_t;


typedef struct {
    uint64_t file_handle;
} __attribute__((packed)) btfs_fsync_req_t;


// Response structures
typedef struct {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t atime_sec;
    uint64_t atime_nsec;
    uint64_t mtime_sec;
    uint64_t mtime_nsec;
    uint64_t ctime_sec;
    uint64_t ctime_nsec;
} __attribute__((packed)) btfs_attr_t;

typedef struct {
    uint64_t file_handle;
    uint32_t lock_acquired;
} __attribute__((packed)) btfs_open_resp_t;

typedef struct {
    uint32_t bytes_read;
    char data[0];
} __attribute__((packed)) btfs_read_resp_t;

typedef struct {
    uint32_t bytes_written;
} __attribute__((packed)) btfs_write_resp_t;

typedef struct {
    uint64_t ino;
    uint32_t type;
    uint32_t name_len;
    char name[0];
} __attribute__((packed)) btfs_dirent_t;

typedef struct {
    uint32_t lock_id;
} __attribute__((packed)) btfs_lock_resp_t;

#endif // BTFS_PROTOCOL_H

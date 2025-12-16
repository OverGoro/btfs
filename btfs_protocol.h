#ifndef BTFS_PROTOCOL_H
#define BTFS_PROTOCOL_H

#include <stdint.h>
#include <sys/stat.h>
#include <linux/limits.h>

#define BTFS_MAX_PATH 256
#define BTFS_MAX_DATA 4096

typedef enum {
    BTFS_OP_GETATTR = 1,    // stat()
    BTFS_OP_READDIR = 2,    // readdir()
    BTFS_OP_ACCESS = 3,     // access()
    BTFS_OP_STATFS = 4,     // statfs()

    BTFS_OP_OPEN = 10,      // open()
    BTFS_OP_READ = 11,      // read()
    BTFS_OP_WRITE = 12,     // write()
    BTFS_OP_CLOSE = 13,     // close()
    BTFS_OP_FSYNC = 14,     // fsync()
    
    BTFS_OP_MKDIR = 20,     // mkdir()
    BTFS_OP_RMDIR = 21,     // rmdir()
    
    BTFS_OP_CREATE = 30,    // creat()
    BTFS_OP_UNLINK = 31,    // unlink()
    BTFS_OP_RENAME = 32,    // rename()
    BTFS_OP_TRUNCATE = 33,  // truncate()
    BTFS_OP_CHMOD = 34,     // chmod()
    BTFS_OP_CHOWN = 35,     // chown()
    BTFS_OP_UTIMENS = 36,   // utimens()
    
    BTFS_OP_LOCK = 40,      // Запрос блокировки
    BTFS_OP_UNLOCK = 41,    // Освобождение блокировки
    
    BTFS_OP_PING = 50,      // Keep-alive
    BTFS_OP_DISCONNECT = 51 // Отключение клиента
} btfs_opcode_t;

// Типы блокировок
typedef enum {
    BTFS_LOCK_NONE = 0,
    BTFS_LOCK_READ = 1,
    BTFS_LOCK_WRITE = 2
} btfs_lock_type_t;

// Заголовок запроса
typedef struct {
    uint32_t opcode;        // Операция
    uint32_t sequence;      // Порядковый номер
    uint32_t client_id;     // ID клиента
    uint32_t flags;         // Флаги операции
    uint32_t data_len;      // Длина данных после заголовка
} btfs_header_t;

// Заголовок ответа
typedef struct {
    uint32_t opcode;
    uint32_t sequence;
    int32_t  result;
    uint32_t data_len;
} btfs_response_t;

// Структуры запросов
typedef struct {
    char path[BTFS_MAX_PATH];
} btfs_getattr_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint64_t offset;        // Offset в директории
} btfs_readdir_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t flags;         // O_RDONLY, O_WRONLY, O_RDWR
    uint32_t mode;          // Permissions
    uint32_t lock_type;     // Тип блокировки (BTFS_LOCK_*)
} btfs_open_req_t;

typedef struct {
    uint64_t file_handle;
    uint64_t offset;
    uint32_t size;
} btfs_read_req_t;

typedef struct {
    uint64_t file_handle;
    uint64_t offset;
    uint32_t size;
    char *data;
} btfs_write_req_t;

typedef struct {
    uint64_t file_handle;
} btfs_close_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t mode;
} btfs_mkdir_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t mode;
    uint32_t flags;
} btfs_create_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
} btfs_unlink_req_t;

typedef struct {
    char oldpath[BTFS_MAX_PATH];
    char newpath[BTFS_MAX_PATH];
} btfs_rename_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t lock_type;  
    uint32_t timeout_sec;
} btfs_lock_req_t;

// Структуры ответов
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
} btfs_attr_t;

typedef struct {
    uint64_t file_handle;
    uint32_t lock_acquired; // 1 если блокировка получена
} btfs_open_resp_t;

typedef struct {
    uint32_t bytes_read;
    char data[0];
} btfs_read_resp_t;

typedef struct {
    uint32_t bytes_written;
} btfs_write_resp_t;

typedef struct {
    uint64_t ino;
    uint32_t type;          // DT_REG, DT_DIR, etc
    uint32_t name_len;
    char name[0];
} btfs_dirent_t;

typedef struct {
    uint32_t lock_id;       // ID блокировки
} btfs_lock_resp_t;

#endif // BTFS_PROTOCOL_H

// btfs_client_fs.c - ПОЛНАЯ версия без зависаний

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/pagemap.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/statfs.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BTFS Team");
MODULE_DESCRIPTION("Bluetooth Distributed File System - Client");

#define BTFS_MAGIC 0x42544653
#define NETLINK_BTFS 31
#define BTFS_MAX_PATH 256
#define BTFS_MAX_DATA 4096
#define BTFS_TIMEOUT_MS 5000

enum {
    BTFS_OP_GETATTR = 1,
    BTFS_OP_READDIR = 2,
    BTFS_OP_OPEN = 10,
    BTFS_OP_READ = 11,
    BTFS_OP_WRITE = 12,
    BTFS_OP_CLOSE = 13,
    BTFS_OP_MKDIR = 20,
    BTFS_OP_RMDIR = 21,
    BTFS_OP_CREATE = 30,
    BTFS_OP_UNLINK = 31,
    BTFS_OP_RENAME = 32,
    BTFS_OP_TRUNCATE = 33
};

typedef struct {
    uint32_t opcode, sequence;
    int32_t result;
    uint32_t data_len;
    uint8_t data[0];
} __packed nl_msg_t;

typedef struct {
    uint64_t ino, size, blocks;
    uint32_t mode, nlink, uid, gid;
    uint64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
} __packed btfs_attr_t;

typedef struct {
    char path[BTFS_MAX_PATH];
} __packed btfs_path_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint64_t offset;
} __packed btfs_readdir_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t flags, mode, lock_type;
} __packed btfs_open_req_t;

typedef struct {
    uint64_t file_handle;
    uint32_t lock_acquired;
} __packed btfs_open_resp_t;

typedef struct {
    uint64_t file_handle, offset;
    uint32_t size;
} __packed btfs_rw_req_t;

typedef struct {
    uint32_t bytes_read;
    char data[0];
} __packed btfs_read_resp_t;

typedef struct {
    uint64_t file_handle, offset;
    uint32_t size;
    char data[0];
} __packed btfs_write_req_t;

typedef struct {
    uint32_t bytes_written;
} __packed btfs_write_resp_t;

typedef struct {
    uint64_t ino;
    uint32_t type, name_len;
    char name[0];
} __packed btfs_dirent_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t mode, flags;
} __packed btfs_create_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint32_t mode;
} __packed btfs_mkdir_req_t;

typedef struct {
    char oldpath[BTFS_MAX_PATH];
    char newpath[BTFS_MAX_PATH];
} __packed btfs_rename_req_t;

typedef struct {
    char path[BTFS_MAX_PATH];
    uint64_t size;
} __packed btfs_truncate_req_t;

typedef struct btfs_pending {
    uint32_t seq;
    wait_queue_head_t wait;
    atomic_t done;
    int32_t res;
    void *data;
    uint32_t len;
    struct btfs_pending *next;
} pending_t;

struct btfs_inode_info {
    uint64_t fh;
    struct inode vfs_inode;
};

static inline struct btfs_inode_info *BTFS_I(struct inode *i) { 
    return container_of(i, struct btfs_inode_info, vfs_inode); 
}

// Глобальные переменные
static struct sock *nl_sock;
static atomic_t daemon_connected = ATOMIC_INIT(0);
static uint32_t daemon_pid = 0;
static atomic_t seq_ctr = ATOMIC_INIT(1);
static DEFINE_MUTEX(nl_mtx);
static DEFINE_SPINLOCK(pend_lock);
static pending_t *pend_head;
static struct kmem_cache *inode_cache;
static atomic_t shutting_down = ATOMIC_INIT(0);

// ============ PENDING REQUEST MANAGEMENT ============

static pending_t *create_pend(uint32_t seq) {
    pending_t *p;
    unsigned long flags;
    
    p = kmalloc(sizeof(*p), GFP_ATOMIC);
    if (!p)
        return NULL;
    
    p->seq = seq;
    atomic_set(&p->done, 0);
    p->res = 0;
    p->data = NULL;
    p->len = 0;
    init_waitqueue_head(&p->wait);
    
    spin_lock_irqsave(&pend_lock, flags);
    p->next = pend_head;
    pend_head = p;
    spin_unlock_irqrestore(&pend_lock, flags);
    
    return p;
}

static pending_t *find_pend(uint32_t seq) {
    pending_t *p;
    unsigned long flags;
    
    spin_lock_irqsave(&pend_lock, flags);
    for (p = pend_head; p && p->seq != seq; p = p->next);
    spin_unlock_irqrestore(&pend_lock, flags);
    
    return p;
}

static void remove_pend(uint32_t seq) {
    pending_t *p, *prev = NULL;
    unsigned long flags;
    
    spin_lock_irqsave(&pend_lock, flags);
    for (p = pend_head; p; prev = p, p = p->next) {
        if (p->seq == seq) {
            if (prev)
                prev->next = p->next;
            else
                pend_head = p->next;
            spin_unlock_irqrestore(&pend_lock, flags);
            
            if (p->data)
                kfree(p->data);
            kfree(p);
            return;
        }
    }
    spin_unlock_irqrestore(&pend_lock, flags);
}

static void abort_all_pending(void) {
    pending_t *p;
    unsigned long flags;
    
    spin_lock_irqsave(&pend_lock, flags);
    p = pend_head;
    while (p) {
        atomic_set(&p->done, 1);
        p->res = -ECONNRESET;
        wake_up_all(&p->wait);
        p = p->next;
    }
    spin_unlock_irqrestore(&pend_lock, flags);
}

// ============ NETLINK COMMUNICATION ============

static int send_nl(uint32_t op, const void *data, uint32_t len, uint32_t *seq) {
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    nl_msg_t *msg;
    int ret;
    
    if (!atomic_read(&daemon_connected) || atomic_read(&shutting_down))
        return -ENOTCONN;
    
    *seq = atomic_inc_return(&seq_ctr);
    
    skb = nlmsg_new(sizeof(nl_msg_t) + len, GFP_KERNEL);
    if (!skb)
        return -ENOMEM;
    
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, sizeof(nl_msg_t) + len, 0);
    if (!nlh) {
        kfree_skb(skb);
        return -ENOMEM;
    }
    
    msg = (nl_msg_t *)nlmsg_data(nlh);
    msg->opcode = op;
    msg->sequence = *seq;
    msg->result = 0;
    msg->data_len = len;
    if (data && len)
        memcpy(msg->data, data, len);
    
    mutex_lock(&nl_mtx);
    if (!daemon_pid || !nl_sock) {
        mutex_unlock(&nl_mtx);
        kfree_skb(skb);
        return -ENOTCONN;
    }
    ret = nlmsg_unicast(nl_sock, skb, daemon_pid);
    mutex_unlock(&nl_mtx);
    
    if (ret < 0) {
        atomic_set(&daemon_connected, 0);
        abort_all_pending();
        return -ENOTCONN;
    }
    
    return 0;
}

static void nl_recv(struct sk_buff *skb) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
    nl_msg_t *msg = (nl_msg_t *)nlmsg_data(nlh);
    pending_t *p;
    
    if (!daemon_pid) {
        daemon_pid = nlh->nlmsg_pid;
        atomic_set(&daemon_connected, 1);
        pr_info("btfs: Daemon connected, PID=%u\n", daemon_pid);
    }
    
    p = find_pend(msg->sequence);
    if (!p)
        return;
    
    p->res = msg->result;
    if (msg->data_len) {
        p->data = kmalloc(msg->data_len, GFP_ATOMIC);
        if (p->data) {
            memcpy(p->data, msg->data, msg->data_len);
            p->len = msg->data_len;
        }
    }
    
    atomic_set(&p->done, 1);
    wake_up_all(&p->wait);
}

static int wait_resp(uint32_t seq, int32_t *res, void *buf, uint32_t sz) {
    pending_t *p = find_pend(seq);
    int ret;
    long timeout;
    
    if (!p)
        return -EINVAL;
    
    timeout = msecs_to_jiffies(BTFS_TIMEOUT_MS);
    
    ret = wait_event_timeout(p->wait, 
                            atomic_read(&p->done) || 
                            atomic_read(&shutting_down) ||
                            !atomic_read(&daemon_connected),
                            timeout);
    
    if (atomic_read(&shutting_down)) {
        remove_pend(seq);
        return -ESHUTDOWN;
    }
    
    if (!atomic_read(&daemon_connected)) {
        remove_pend(seq);
        return -ECONNRESET;
    }
    
    if (ret == 0) {
        pr_warn("btfs: Request seq=%u timed out after %dms\n", seq, BTFS_TIMEOUT_MS);
        remove_pend(seq);
        return -ETIMEDOUT;
    }
    
    *res = p->res;
    if (buf && sz && p->data && p->len)
        memcpy(buf, p->data, min(p->len, sz));
    
    remove_pend(seq);
    return 0;
}

// ============ HELPER FUNCTIONS ============

static void get_path(struct dentry *d, char *buf, size_t sz) {
    char *tmp = kmalloc(PATH_MAX, GFP_KERNEL), *path;
    if (!tmp) {
        strncpy(buf, ".", sz);
        return;
    }
    
    path = dentry_path_raw(d, tmp, PATH_MAX);
    if (IS_ERR(path))
        strncpy(buf, ".", sz);
    else {
        while (*path == '/')
            path++;
        strncpy(buf, *path ? path : ".", sz - 1);
        buf[sz - 1] = 0;
    }
    kfree(tmp);
}

// ============ FILE OPERATIONS ============

static int btfs_open(struct inode *i, struct file *f) {
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_open_req_t req;
    btfs_open_resp_t resp;
    uint32_t seq;
    int32_t res;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    get_path(f->f_path.dentry, req.path, BTFS_MAX_PATH);
    req.flags = f->f_flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_TRUNC);
    req.mode = 0;
    req.lock_type = 0;
    
    ret = send_nl(BTFS_OP_OPEN, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, &resp, sizeof(resp));
    if (ret < 0)
        return ret;
    if (res < 0)
        return res;
    
    info->fh = resp.file_handle;
    return 0;
}

static int btfs_release(struct inode *i, struct file *f) {
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_rw_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
    
    if (!info->fh)
        return 0;
    
    req.file_handle = info->fh;
    
    if (atomic_read(&daemon_connected) && !atomic_read(&shutting_down)) {
        ret = send_nl(BTFS_OP_CLOSE, &req, sizeof(uint64_t), &seq);
        if (ret == 0 && create_pend(seq)) {
            wait_resp(seq, &res, NULL, 0);
        }
    }
    
    info->fh = 0;
    return 0;
}

static void btfs_evict_inode(struct inode *inode) {
    struct btfs_inode_info *info = BTFS_I(inode);
    
    truncate_inode_pages_final(&inode->i_data);
    clear_inode(inode);
    
    if (info->fh) {
        if (!atomic_read(&daemon_connected) || atomic_read(&shutting_down)) {
            pr_debug("btfs: Inode evicted with leaked fh=%llu (daemon disconnected)\n", 
                    info->fh);
        } else {
            pr_warn("btfs: Inode evicted with open fh=%llu\n", info->fh);
        }
        info->fh = 0;
    }
}

static ssize_t btfs_read(struct file *f, char __user *buf, size_t len, loff_t *off) {
    struct inode *i = file_inode(f);
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_rw_req_t req;
    char *rbuf;
    btfs_read_resp_t *resp;
    uint32_t seq;
    int32_t res;
    int ret;
    ssize_t total_read = 0;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    if (!info->fh) {
        ret = btfs_open(i, f);
        if (ret < 0)
            return ret;
    }
    
    while (len > 0) {
        uint32_t chunk_size = (len > 4000) ? 4000 : len;
        
        req.file_handle = info->fh;
        req.offset = *off;
        req.size = chunk_size;
        
        ret = send_nl(BTFS_OP_READ, &req, sizeof(req), &seq);
        if (ret < 0)
            return total_read > 0 ? total_read : ret;
        
        if (!create_pend(seq))
            return total_read > 0 ? total_read : -ENOMEM;
        
        rbuf = kmalloc(sizeof(btfs_read_resp_t) + chunk_size, GFP_KERNEL);
        if (!rbuf) {
            remove_pend(seq);
            return total_read > 0 ? total_read : -ENOMEM;
        }
        
        ret = wait_resp(seq, &res, rbuf, sizeof(btfs_read_resp_t) + chunk_size);
        if (ret < 0 || res < 0) {
            kfree(rbuf);
            return total_read > 0 ? total_read : (ret < 0 ? ret : res);
        }
        
        resp = (btfs_read_resp_t *)rbuf;
        ssize_t bytes = resp->bytes_read;
        
        if (bytes > 0) {
            if (copy_to_user(buf + total_read, resp->data, bytes)) {
                kfree(rbuf);
                return total_read > 0 ? total_read : -EFAULT;
            }
            total_read += bytes;
            *off += bytes;
            len -= bytes;
            
            if (bytes < chunk_size) {
                kfree(rbuf);
                break;
            }
        } else {
            kfree(rbuf);
            break;
        }
        kfree(rbuf);
    }
    
    return total_read;
}

static ssize_t btfs_write(struct file *f, const char __user *buf, size_t len, loff_t *off) {
    struct inode *i = file_inode(f);
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_write_req_t *req;
    btfs_write_resp_t resp;
    uint32_t seq;
    int32_t res;
    int ret;
    size_t sz;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    if (!info->fh) {
        ret = btfs_open(i, f);
        if (ret < 0)
            return ret;
    }
    
    sz = sizeof(btfs_write_req_t) + len;
    req = kmalloc(sz, GFP_KERNEL);
    if (!req)
        return -ENOMEM;
    
    req->file_handle = info->fh;
    req->offset = *off;
    req->size = len;
    
    if (copy_from_user(req->data, buf, len)) {
        kfree(req);
        return -EFAULT;
    }
    
    ret = send_nl(BTFS_OP_WRITE, req, sz, &seq);
    kfree(req);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, &resp, sizeof(resp));
    if (ret < 0)
        return ret;
    if (res < 0)
        return res;
    
    *off += resp.bytes_written;
    return resp.bytes_written;
}

static int btfs_readdir(struct file *f, struct dir_context *ctx) {
    btfs_readdir_req_t req;
    char *buf;
    uint32_t seq;
    int32_t res;
    int ret;
    size_t off;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    if (ctx->pos == 0) {
        if (!dir_emit_dot(f, ctx))
            return 0;
        ctx->pos++;
    }
    
    if (ctx->pos == 1) {
        if (!dir_emit_dotdot(f, ctx))
            return 0;
        ctx->pos++;
    }
    
    if (ctx->pos == 2) {
        get_path(f->f_path.dentry, req.path, BTFS_MAX_PATH);
        req.offset = 0;
        
        ret = send_nl(BTFS_OP_READDIR, &req, sizeof(req), &seq);
        if (ret < 0)
            return ret;
        
        if (!create_pend(seq))
            return -ENOMEM;
        
        buf = kmalloc(BTFS_MAX_DATA, GFP_KERNEL);
        if (!buf) {
            remove_pend(seq);
            return -ENOMEM;
        }
        
        ret = wait_resp(seq, &res, buf, BTFS_MAX_DATA);
        if (ret < 0 || res < 0) {
            kfree(buf);
            return ret < 0 ? ret : res;
        }
        
        for (off = 0; off < BTFS_MAX_DATA;) {
            btfs_dirent_t *e = (btfs_dirent_t *)(buf + off);
            if (!e->name_len || e->name_len > 255)
                break;
            if (!dir_emit(ctx, e->name, e->name_len, e->ino, e->type))
                break;
            off += sizeof(btfs_dirent_t) + e->name_len + 1;
        }
        
        kfree(buf);
        ctx->pos++;
    }
    
    return 0;
}

// ============ INODE OPERATIONS ============

static struct inode *btfs_make_inode(struct super_block *sb, umode_t mode);

static int btfs_getattr(struct mnt_idmap *idmap, const struct path *path,
                        struct kstat *stat, u32 mask, unsigned int flags) {
    struct inode *i = d_inode(path->dentry);
    btfs_path_req_t req;
    btfs_attr_t attr;
    uint32_t seq;
    int32_t res;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    get_path(path->dentry, req.path, BTFS_MAX_PATH);
    ret = send_nl(BTFS_OP_GETATTR, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, &attr, sizeof(attr));
    if (ret < 0)
        return ret;
    if (res < 0)
        return res;
    
    generic_fillattr(idmap, mask, i, stat);
    stat->ino = attr.ino;
    stat->size = attr.size;
    stat->blocks = attr.blocks;
    stat->mode = attr.mode;
    stat->nlink = attr.nlink;
    stat->uid = KUIDT_INIT(attr.uid);
    stat->gid = KGIDT_INIT(attr.gid);
    stat->atime.tv_sec = attr.atime_sec;
    stat->atime.tv_nsec = attr.atime_nsec;
    stat->mtime.tv_sec = attr.mtime_sec;
    stat->mtime.tv_nsec = attr.mtime_nsec;
    stat->ctime.tv_sec = attr.ctime_sec;
    stat->ctime.tv_nsec = attr.ctime_nsec;
    
    return 0;
}

static struct dentry *btfs_lookup(struct inode *dir, struct dentry *d, unsigned int flags) {
    btfs_path_req_t req;
    btfs_attr_t attr;
    uint32_t seq;
    int32_t res;
    struct inode *i;
    int ret;
    
    if (atomic_read(&shutting_down))
        return ERR_PTR(-ESHUTDOWN);
    
    get_path(d, req.path, BTFS_MAX_PATH);
    ret = send_nl(BTFS_OP_GETATTR, &req, sizeof(req), &seq);
    if (ret < 0)
        return ERR_PTR(ret);
    
    if (!create_pend(seq))
        return ERR_PTR(-ENOMEM);
    
    ret = wait_resp(seq, &res, &attr, sizeof(attr));
    if (ret < 0)
        return ERR_PTR(ret);
    
    if (res == -ENOENT) {
        d_add(d, NULL);
        return NULL;
    }
    
    if (res < 0)
        return ERR_PTR(res);
    
    i = btfs_make_inode(dir->i_sb, attr.mode);
    if (!i)
        return ERR_PTR(-ENOMEM);
    
    i->i_ino = attr.ino;
    i->i_size = attr.size;
    i->i_blocks = attr.blocks;
    set_nlink(i, attr.nlink);
    i->i_uid = KUIDT_INIT(attr.uid);
    i->i_gid = KGIDT_INIT(attr.gid);
    
    d_add(d, i);
    return NULL;
}

static int btfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *d,
                       umode_t mode, bool excl) {
    btfs_create_req_t req;
    uint32_t seq;
    int32_t res;
    struct inode *i;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    get_path(d, req.path, BTFS_MAX_PATH);
    req.mode = mode;
    req.flags = 0;
    
    ret = send_nl(BTFS_OP_CREATE, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, NULL, 0);
    if (ret < 0)
        return ret;
    if (res < 0)
        return res;
    
    i = btfs_make_inode(dir->i_sb, mode);
    if (!i)
        return -ENOMEM;
    
    d_instantiate(d, i);
    return 0;
}

static int btfs_unlink(struct inode *dir, struct dentry *d) {
    btfs_path_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    get_path(d, req.path, BTFS_MAX_PATH);
    ret = send_nl(BTFS_OP_UNLINK, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, NULL, 0);
    return ret < 0 ? ret : res;
}

static int btfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *d, umode_t mode) {
    btfs_mkdir_req_t req;
    uint32_t seq;
    int32_t res;
    struct inode *i;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    get_path(d, req.path, BTFS_MAX_PATH);
    req.mode = mode;
    
    ret = send_nl(BTFS_OP_MKDIR, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, NULL, 0);
    if (ret < 0)
        return ret;
    if (res < 0)
        return res;
    
    i = btfs_make_inode(dir->i_sb, S_IFDIR | mode);
    if (!i)
        return -ENOMEM;
    
    d_instantiate(d, i);
    inc_nlink(dir);
    return 0;
}

static int btfs_rmdir(struct inode *dir, struct dentry *d) {
    btfs_path_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    get_path(d, req.path, BTFS_MAX_PATH);
    ret = send_nl(BTFS_OP_RMDIR, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, NULL, 0);
    if (ret < 0)
        return ret;
    
    if (res == 0)
        drop_nlink(dir);
    return res;
}

static int btfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
                       struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
    btfs_rename_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    
    get_path(old_dentry, req.oldpath, BTFS_MAX_PATH);
    get_path(new_dentry, req.newpath, BTFS_MAX_PATH);
    
    ret = send_nl(BTFS_OP_RENAME, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    
    if (!create_pend(seq))
        return -ENOMEM;
    
    ret = wait_resp(seq, &res, NULL, 0);
    return ret < 0 ? ret : res;
}

static int btfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr) {
    struct inode *inode = d_inode(dentry);
    int ret;
    
    if (atomic_read(&shutting_down))
        return -ESHUTDOWN;
    
    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;
    
    if (attr->ia_valid & ATTR_SIZE) {
        btfs_truncate_req_t req;
        uint32_t seq;
        int32_t res;
        
        get_path(dentry, req.path, BTFS_MAX_PATH);
        req.size = attr->ia_size;
        
        ret = send_nl(BTFS_OP_TRUNCATE, &req, sizeof(req), &seq);
        if (ret < 0)
            return ret;
        
        if (!create_pend(seq))
            return -ENOMEM;
        
        ret = wait_resp(seq, &res, NULL, 0);
        if (ret < 0)
            return ret;
        if (res < 0)
            return res;
        
        truncate_setsize(inode, attr->ia_size);
    }
    
    setattr_copy(idmap, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

// ============ OPERATIONS TABLES ============

static const struct file_operations btfs_file_ops = {
    .open = btfs_open,
    .release = btfs_release,
    .read = btfs_read,
    .write = btfs_write,
    .llseek = generic_file_llseek,
    .fsync = noop_fsync
};

static const struct file_operations btfs_dir_ops = {
    .iterate_shared = btfs_readdir,
    .llseek = generic_file_llseek
};

static const struct inode_operations btfs_file_inode_ops = {
    .getattr = btfs_getattr,
    .setattr = btfs_setattr
};

static const struct inode_operations btfs_dir_inode_ops = {
    .lookup = btfs_lookup,
    .getattr = btfs_getattr,
    .create = btfs_create,
    .unlink = btfs_unlink,
    .mkdir = btfs_mkdir,
    .rmdir = btfs_rmdir,
    .rename = btfs_rename
};

// ============ SUPERBLOCK OPERATIONS ============

static struct inode *btfs_alloc_inode(struct super_block *sb) {
    struct btfs_inode_info *info = kmem_cache_alloc(inode_cache, GFP_KERNEL);
    if (!info)
        return NULL;
    info->fh = 0;
    return &info->vfs_inode;
}

static void btfs_free_inode(struct inode *i) {
    kmem_cache_free(inode_cache, BTFS_I(i));
}

static void btfs_put_super(struct super_block *sb) {
    pr_info("btfs: Superblock cleanup\n");
    abort_all_pending();
}

static const struct super_operations btfs_super_ops = {
    .alloc_inode = btfs_alloc_inode,
    .free_inode = btfs_free_inode,
    .evict_inode = btfs_evict_inode,
    .put_super = btfs_put_super,
    .statfs = simple_statfs,
    .drop_inode = generic_delete_inode
};

static struct inode *btfs_make_inode(struct super_block *sb, umode_t mode) {
    struct inode *i = new_inode(sb);
    struct timespec64 ts;
    
    if (i) {
        i->i_ino = get_next_ino();
        i->i_mode = mode;
        i->i_uid = current_fsuid();
        i->i_gid = current_fsgid();
        ts = current_time(i);
        inode_set_atime_to_ts(i, ts);
        inode_set_mtime_to_ts(i, ts);
        inode_set_ctime_to_ts(i, ts);
        
        if (S_ISDIR(mode)) {
            i->i_op = &btfs_dir_inode_ops;
            i->i_fop = &btfs_dir_ops;
            inc_nlink(i);
        } else {
            i->i_op = &btfs_file_inode_ops;
            i->i_fop = &btfs_file_ops;
        }
    }
    return i;
}

static int btfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *root;
    struct dentry *d;
    
    sb->s_magic = BTFS_MAGIC;
    sb->s_op = &btfs_super_ops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    
    root = btfs_make_inode(sb, S_IFDIR | 0755);
    if (!root)
        return -ENOMEM;
    
    d = d_make_root(root);
    if (!d)
        return -ENOMEM;
    
    sb->s_root = d;
    pr_info("btfs: Filesystem mounted\n");
    return 0;
}

static struct dentry *btfs_mount(struct file_system_type *t, int flags, 
                                  const char *dev, void *data) {
    if (!atomic_read(&daemon_connected)) {
        pr_err("btfs: Cannot mount - daemon not connected\n");
        return ERR_PTR(-ENOTCONN);
    }
    return mount_nodev(t, flags, data, btfs_fill_super);
}

static void btfs_kill_sb(struct super_block *sb) {
    kill_litter_super(sb);
    pr_info("btfs: Filesystem unmounted\n");
}

static struct file_system_type btfs_fs = {
    .owner = THIS_MODULE,
    .name = "btfs",
    .mount = btfs_mount,
    .kill_sb = btfs_kill_sb,
    .fs_flags = FS_USERNS_MOUNT
};

// ============ MODULE INIT/EXIT ============

static int __init btfs_init(void) {
    int ret;
    struct netlink_kernel_cfg cfg = {.input = nl_recv};
    
    pr_info("btfs: Initializing module (v2.0 - no-hang edition)\n");
    
    inode_cache = kmem_cache_create("btfs_inode_cache", 
                                    sizeof(struct btfs_inode_info), 
                                    0, SLAB_RECLAIM_ACCOUNT, NULL);
    if (!inode_cache) {
        pr_err("btfs: Failed to create inode cache\n");
        return -ENOMEM;
    }
    
    nl_sock = netlink_kernel_create(&init_net, NETLINK_BTFS, &cfg);
    if (!nl_sock) {
        pr_err("btfs: Failed to create netlink socket\n");
        kmem_cache_destroy(inode_cache);
        return -ENOMEM;
    }
    
    ret = register_filesystem(&btfs_fs);
    if (ret) {
        pr_err("btfs: Failed to register filesystem\n");
        netlink_kernel_release(nl_sock);
        kmem_cache_destroy(inode_cache);
        return ret;
    }
    
    pr_info("btfs: Module loaded - waiting for daemon\n");
    return 0;
}

static void __exit btfs_exit(void) {
    pending_t *p, *next;
    unsigned long flags;
    
    pr_info("btfs: Shutting down module\n");
    
    atomic_set(&shutting_down, 1);
    atomic_set(&daemon_connected, 0);
    
    unregister_filesystem(&btfs_fs);
    
    abort_all_pending();
    
    msleep(1000);
    
    if (nl_sock) {
        netlink_kernel_release(nl_sock);
        nl_sock = NULL;
    }
    
    spin_lock_irqsave(&pend_lock, flags);
    p = pend_head;
    pend_head = NULL;
    spin_unlock_irqrestore(&pend_lock, flags);
    
    while (p) {
        next = p->next;
        if (p->data)
            kfree(p->data);
        kfree(p);
        p = next;
    }
    
    rcu_barrier();
    
    if (inode_cache) {
        kmem_cache_destroy(inode_cache);
        inode_cache = NULL;
    }
    
    pr_info("btfs: Module unloaded cleanly\n");
}

module_init(btfs_init);
module_exit(btfs_exit);

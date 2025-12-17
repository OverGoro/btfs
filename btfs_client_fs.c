// btfs_client_fs.c - Полная версия со всеми операциями
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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BTFS Team");
MODULE_DESCRIPTION("Bluetooth Distributed File System - Client");

#define BTFS_MAGIC 0x42544653
#define NETLINK_BTFS 31
#define BTFS_MAX_PATH 256
#define BTFS_MAX_DATA 4096

enum
{
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

typedef struct
{
    uint32_t opcode, sequence;
    int32_t result;
    uint32_t data_len;
    uint8_t data[0];
} __packed nl_msg_t;
typedef struct
{
    uint64_t ino, size, blocks;
    uint32_t mode, nlink, uid, gid;
    uint64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
} __packed btfs_attr_t;
typedef struct
{
    char path[BTFS_MAX_PATH];
} __packed btfs_path_req_t;
typedef struct
{
    char path[BTFS_MAX_PATH];
    uint64_t offset;
} __packed btfs_readdir_req_t;
typedef struct
{
    char path[BTFS_MAX_PATH];
    uint32_t flags, mode, lock_type;
} __packed btfs_open_req_t;
typedef struct
{
    uint64_t file_handle;
    uint32_t lock_acquired;
} __packed btfs_open_resp_t;
typedef struct
{
    uint64_t file_handle, offset;
    uint32_t size;
} __packed btfs_rw_req_t;
typedef struct
{
    uint32_t bytes_read;
    char data[0];
} __packed btfs_read_resp_t;
typedef struct
{
    uint64_t file_handle, offset;
    uint32_t size;
    char data[0];
} __packed btfs_write_req_t;
typedef struct
{
    uint32_t bytes_written;
} __packed btfs_write_resp_t;
typedef struct
{
    uint64_t ino;
    uint32_t type, name_len;
    char name[0];
} __packed btfs_dirent_t;
typedef struct
{
    char path[BTFS_MAX_PATH];
    uint32_t mode, flags;
} __packed btfs_create_req_t;
typedef struct
{
    char path[BTFS_MAX_PATH];
    uint32_t mode;
} __packed btfs_mkdir_req_t;
typedef struct
{
    char oldpath[BTFS_MAX_PATH];
    char newpath[BTFS_MAX_PATH];
} __packed btfs_rename_req_t;
typedef struct
{
    char path[BTFS_MAX_PATH];
    uint64_t size;
} __packed btfs_truncate_req_t;

typedef struct btfs_pending
{
    uint32_t seq;
    wait_queue_head_t wait;
    int done;
    int32_t res;
    void *data;
    uint32_t len;
    struct btfs_pending *next;
} pending_t;

struct btfs_inode_info
{
    uint64_t fh;
    struct inode vfs_inode;
};
static inline struct btfs_inode_info *BTFS_I(struct inode *i) { return container_of(i, struct btfs_inode_info, vfs_inode); }

static struct sock *nl_sock;
static uint32_t daemon_pid, seq_ctr = 1;
static DEFINE_MUTEX(nl_mtx);
static DEFINE_MUTEX(pend_mtx);
static pending_t *pend_head;
static struct kmem_cache *inode_cache;

static pending_t *create_pend(uint32_t seq)
{
    pending_t *p = kmalloc(sizeof(*p), GFP_KERNEL);
    if (!p)
        return NULL;
    p->seq = seq;
    p->done = 0;
    p->res = 0;
    p->data = NULL;
    p->len = 0;
    init_waitqueue_head(&p->wait);
    mutex_lock(&pend_mtx);
    p->next = pend_head;
    pend_head = p;
    mutex_unlock(&pend_mtx);
    return p;
}

static pending_t *find_pend(uint32_t seq)
{
    pending_t *p;
    mutex_lock(&pend_mtx);
    for (p = pend_head; p && p->seq != seq; p = p->next)
        ;
    mutex_unlock(&pend_mtx);
    return p;
}

static void remove_pend(uint32_t seq)
{
    pending_t *p, *prev = NULL;
    mutex_lock(&pend_mtx);
    for (p = pend_head; p; prev = p, p = p->next)
    {
        if (p->seq == seq)
        {
            if (prev)
                prev->next = p->next;
            else
                pend_head = p->next;
            if (p->data)
                kfree(p->data);
            kfree(p);
            break;
        }
    }
    mutex_unlock(&pend_mtx);
}

static int send_nl(uint32_t op, const void *data, uint32_t len, uint32_t *seq)
{
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    nl_msg_t *msg;
    int ret;
    if (!daemon_pid)
        return -ENOTCONN;
    mutex_lock(&nl_mtx);
    *seq = seq_ctr++;
    skb = nlmsg_new(sizeof(nl_msg_t) + len, GFP_KERNEL);
    if (!skb)
    {
        mutex_unlock(&nl_mtx);
        return -ENOMEM;
    }
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, sizeof(nl_msg_t) + len, 0);
    if (!nlh)
    {
        kfree_skb(skb);
        mutex_unlock(&nl_mtx);
        return -ENOMEM;
    }
    msg = (nl_msg_t *)nlmsg_data(nlh);
    msg->opcode = op;
    msg->sequence = *seq;
    msg->result = 0;
    msg->data_len = len;
    if (data && len)
        memcpy(msg->data, data, len);
    ret = nlmsg_unicast(nl_sock, skb, daemon_pid);
    mutex_unlock(&nl_mtx);
    return ret < 0 ? ret : 0;
}

static void nl_recv(struct sk_buff *skb)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
    nl_msg_t *msg = (nl_msg_t *)nlmsg_data(nlh);
    pending_t *p;
    if (!daemon_pid)
    {
        daemon_pid = nlh->nlmsg_pid;
        pr_info("btfs: Daemon PID=%u\n", daemon_pid);
    }
    p = find_pend(msg->sequence);
    if (!p)
        return;
    p->res = msg->result;
    if (msg->data_len)
    {
        p->data = kmalloc(msg->data_len, GFP_KERNEL);
        if (p->data)
        {
            memcpy(p->data, msg->data, msg->data_len);
            p->len = msg->data_len;
        }
    }
    p->done = 1;
    wake_up_interruptible(&p->wait);
}

static int wait_resp(uint32_t seq, int32_t *res, void *buf, uint32_t sz)
{
    pending_t *p = find_pend(seq);
    int ret;
    if (!p)
        return -EINVAL;
    ret = wait_event_interruptible_timeout(p->wait, p->done, 30 * HZ);
    if (ret <= 0)
    {
        remove_pend(seq);
        return ret == 0 ? -ETIMEDOUT : ret;
    }
    *res = p->res;
    if (buf && sz && p->data && p->len)
        memcpy(buf, p->data, min(p->len, sz));
    remove_pend(seq);
    return 0;
}

static void get_path(struct dentry *d, char *buf, size_t sz)
{
    char *tmp = kmalloc(PATH_MAX, GFP_KERNEL), *path;
    if (!tmp)
    {
        strncpy(buf, ".", sz);
        return;
    }
    path = dentry_path_raw(d, tmp, PATH_MAX);
    if (IS_ERR(path))
        strncpy(buf, ".", sz);
    else
    {
        while (*path == '/')
            path++;
        strncpy(buf, *path ? path : ".", sz - 1);
        buf[sz - 1] = 0;
    }
    kfree(tmp);
}

static int btfs_open(struct inode *i, struct file *f)
{
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_open_req_t req;
    btfs_open_resp_t resp;
    uint32_t seq;
    int32_t res;
    int ret;

    get_path(f->f_path.dentry, req.path, BTFS_MAX_PATH);
    req.flags = f->f_flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_TRUNC); // ИСПРАВЛЕНО: добавлены флаги
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

static int btfs_release(struct inode *i, struct file *f)
{
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_rw_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;

    if (!info->fh)
        return 0;

    req.file_handle = info->fh;
    ret = send_nl(BTFS_OP_CLOSE, &req, sizeof(uint64_t), &seq);
    if (ret < 0)
    {
        info->fh = 0; // Очистить даже при ошибке
        return ret;
    }

    if (!create_pend(seq))
    {
        info->fh = 0;
        return -ENOMEM;
    }

    ret = wait_resp(seq, &res, NULL, 0);
    info->fh = 0; // ВАЖНО: всегда очищать handle

    return ret < 0 ? ret : (res < 0 ? res : 0);
}

static void btfs_evict_inode(struct inode *inode)
{
    struct btfs_inode_info *info = BTFS_I(inode);

    truncate_inode_pages_final(&inode->i_data);
    clear_inode(inode);

    // Если есть открытый handle - закрыть принудительно
    if (info->fh)
    {
        pr_warn("btfs: Force closing file handle %llu\n", info->fh);
        btfs_rw_req_t req;
        uint32_t seq;
        int32_t res;

        req.file_handle = info->fh;
        if (send_nl(BTFS_OP_CLOSE, &req, sizeof(uint64_t), &seq) == 0)
        {
            if (create_pend(seq))
            {
                wait_resp(seq, &res, NULL, 0);
            }
        }
        info->fh = 0;
    }
}

static ssize_t btfs_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    struct inode *i = file_inode(f);
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_rw_req_t req;
    char *rbuf;
    btfs_read_resp_t *resp;
    uint32_t seq;
    int32_t res;
    int ret;
    ssize_t total_read = 0;

    if (!info->fh)
    {
        ret = btfs_open(i, f);
        if (ret < 0)
            return ret;
    }

    // ИСПРАВЛЕНИЕ: читать циклом, пока не прочитаем всё запрошенное или EOF
    while (len > 0)
    {
        // Ограничить размер одного запроса
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
        if (!rbuf)
        {
            remove_pend(seq);
            return total_read > 0 ? total_read : -ENOMEM;
        }

        ret = wait_resp(seq, &res, rbuf, sizeof(btfs_read_resp_t) + chunk_size);
        if (ret < 0 || res < 0)
        {
            kfree(rbuf);
            return total_read > 0 ? total_read : (ret < 0 ? ret : res);
        }

        resp = (btfs_read_resp_t *)rbuf;
        ssize_t bytes = resp->bytes_read;

        if (bytes > 0)
        {
            if (copy_to_user(buf + total_read, resp->data, bytes))
            {
                kfree(rbuf);
                return total_read > 0 ? total_read : -EFAULT;
            }

            total_read += bytes;
            *off += bytes;
            len -= bytes;

            // Если прочитали меньше, чем запросили - достигли EOF
            if (bytes < chunk_size)
            {
                kfree(rbuf);
                break;
            }
        }
        else
        {
            // EOF или ошибка
            kfree(rbuf);
            break;
        }

        kfree(rbuf);
    }

    return total_read;
}

static ssize_t btfs_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
    struct inode *i = file_inode(f);
    struct btfs_inode_info *info = BTFS_I(i);
    btfs_write_req_t *req;
    btfs_write_resp_t resp;
    uint32_t seq;
    int32_t res;
    int ret;
    size_t sz;
    if (!info->fh)
    {
        ret = btfs_open(i, f);
        if (ret < 0)
            return ret;
    }

    sz = sizeof(btfs_write_req_t) + len;
    req = kmalloc(sz, GFP_KERNEL);
    if (!req)
        return -ENOMEM;
    req->file_handle = info->fh;
    req->offset = *off; // Используем текущий offset (VFS обрабатывает O_APPEND автоматически)
    req->size = len;
    if (copy_from_user(req->data, buf, len))
    {
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

static int btfs_readdir(struct file *f, struct dir_context *ctx)
{
    btfs_readdir_req_t req;
    char *buf;
    uint32_t seq;
    int32_t res;
    int ret;
    size_t off;
    if (ctx->pos == 0)
    {
        if (!dir_emit_dot(f, ctx))
            return 0;
        ctx->pos++;
    }
    if (ctx->pos == 1)
    {
        if (!dir_emit_dotdot(f, ctx))
            return 0;
        ctx->pos++;
    }
    if (ctx->pos == 2)
    {
        get_path(f->f_path.dentry, req.path, BTFS_MAX_PATH);
        req.offset = 0;
        ret = send_nl(BTFS_OP_READDIR, &req, sizeof(req), &seq);
        if (ret < 0)
            return ret;
        if (!create_pend(seq))
            return -ENOMEM;
        buf = kmalloc(BTFS_MAX_DATA, GFP_KERNEL);
        if (!buf)
        {
            remove_pend(seq);
            return -ENOMEM;
        }
        ret = wait_resp(seq, &res, buf, BTFS_MAX_DATA);
        if (ret < 0 || res < 0)
        {
            kfree(buf);
            return ret < 0 ? ret : res;
        }
        for (off = 0; off < BTFS_MAX_DATA;)
        {
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

static int btfs_getattr(struct mnt_idmap *idmap, const struct path *path,
                        struct kstat *stat, u32 mask, unsigned int flags)
{
    struct inode *i = d_inode(path->dentry);
    btfs_path_req_t req;
    btfs_attr_t attr;
    uint32_t seq;
    int32_t res;
    int ret;
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

static struct inode *btfs_make_inode(struct super_block *sb, umode_t mode);

static struct dentry *btfs_lookup(struct inode *dir, struct dentry *d, unsigned int flags)
{
    btfs_path_req_t req;
    btfs_attr_t attr;
    uint32_t seq;
    int32_t res;
    struct inode *i;
    int ret;
    get_path(d, req.path, BTFS_MAX_PATH);
    ret = send_nl(BTFS_OP_GETATTR, &req, sizeof(req), &seq);
    if (ret < 0)
        return ERR_PTR(ret);
    if (!create_pend(seq))
        return ERR_PTR(-ENOMEM);
    ret = wait_resp(seq, &res, &attr, sizeof(attr));
    if (ret < 0)
        return ERR_PTR(ret);
    if (res == -ENOENT)
    {
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
                       umode_t mode, bool excl)
{
    btfs_create_req_t req;
    uint32_t seq;
    int32_t res;
    struct inode *i;
    int ret;
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

static int btfs_unlink(struct inode *dir, struct dentry *d)
{
    btfs_path_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
    get_path(d, req.path, BTFS_MAX_PATH);
    ret = send_nl(BTFS_OP_UNLINK, &req, sizeof(req), &seq);
    if (ret < 0)
        return ret;
    if (!create_pend(seq))
        return -ENOMEM;
    ret = wait_resp(seq, &res, NULL, 0);
    return ret < 0 ? ret : res;
}

static int btfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *d, umode_t mode)
{
    btfs_mkdir_req_t req;
    uint32_t seq;
    int32_t res;
    struct inode *i;
    int ret;
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

static int btfs_rmdir(struct inode *dir, struct dentry *d)
{
    btfs_path_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
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
                       struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
    btfs_rename_req_t req;
    uint32_t seq;
    int32_t res;
    int ret;
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

static int btfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    int ret;

    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;

    if (attr->ia_valid & ATTR_SIZE)
    {
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

static const struct file_operations btfs_file_ops = {
    .open = btfs_open, .release = btfs_release, .read = btfs_read, .write = btfs_write, .llseek = generic_file_llseek, .fsync = noop_fsync};
static const struct file_operations btfs_dir_ops = {
    .iterate_shared = btfs_readdir, .llseek = generic_file_llseek};
static const struct inode_operations btfs_file_inode_ops = {
    .getattr = btfs_getattr, .setattr = btfs_setattr};
static const struct inode_operations btfs_dir_inode_ops = {
    .lookup = btfs_lookup, .getattr = btfs_getattr, .create = btfs_create, .unlink = btfs_unlink, .mkdir = btfs_mkdir, .rmdir = btfs_rmdir, .rename = btfs_rename};

static struct inode *btfs_alloc_inode(struct super_block *sb)
{
    struct btfs_inode_info *info = kmem_cache_alloc(inode_cache, GFP_KERNEL);
    if (!info)
        return NULL;
    info->fh = 0;
    return &info->vfs_inode;
}

static void btfs_free_inode(struct inode *i) { kmem_cache_free(inode_cache, BTFS_I(i)); }

static const struct super_operations btfs_super_ops = {
    .alloc_inode = btfs_alloc_inode,
    .free_inode = btfs_free_inode,
    .evict_inode = btfs_evict_inode, // ДОБАВИТЬ
    .statfs = simple_statfs,
    .drop_inode = generic_delete_inode};

static struct inode *btfs_make_inode(struct super_block *sb, umode_t mode)
{
    struct inode *i = new_inode(sb);
    struct timespec64 ts;
    if (i)
    {
        i->i_ino = get_next_ino();
        i->i_mode = mode;
        i->i_uid = current_fsuid();
        i->i_gid = current_fsgid();
        ts = current_time(i);
        inode_set_atime_to_ts(i, ts);
        inode_set_mtime_to_ts(i, ts);
        inode_set_ctime_to_ts(i, ts);
        if (S_ISDIR(mode))
        {
            i->i_op = &btfs_dir_inode_ops;
            i->i_fop = &btfs_dir_ops;
            inc_nlink(i);
        }
        else
        {
            i->i_op = &btfs_file_inode_ops;
            i->i_fop = &btfs_file_ops;
        }
    }
    return i;
}

static int btfs_fill_super(struct super_block *sb, void *data, int silent)
{
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
    pr_info("btfs: Mounted\n");
    return 0;
}

static struct dentry *btfs_mount(struct file_system_type *t, int flags, const char *dev, void *data)
{
    return mount_nodev(t, flags, data, btfs_fill_super);
}

static void btfs_kill_sb(struct super_block *sb)
{
    kill_litter_super(sb);
    pr_info("btfs: Unmounted\n");
}

static struct file_system_type btfs_fs = {
    .owner = THIS_MODULE, .name = "btfs", .mount = btfs_mount, .kill_sb = btfs_kill_sb, .fs_flags = FS_USERNS_MOUNT};

static int __init btfs_init(void)
{
    int ret;
    struct netlink_kernel_cfg cfg = {.input = nl_recv};
    inode_cache = kmem_cache_create("btfs_inode_cache", sizeof(struct btfs_inode_info), 0, 0, NULL);
    if (!inode_cache)
        return -ENOMEM;
    nl_sock = netlink_kernel_create(&init_net, NETLINK_BTFS, &cfg);
    if (!nl_sock)
    {
        kmem_cache_destroy(inode_cache);
        return -ENOMEM;
    }
    ret = register_filesystem(&btfs_fs);
    if (ret)
    {
        netlink_kernel_release(nl_sock);
        kmem_cache_destroy(inode_cache);
        return ret;
    }
    pr_info("btfs: Module loaded\n");
    return 0;
}

static void __exit btfs_exit(void)
{
    pending_t *p;

    unregister_filesystem(&btfs_fs);

    // Сначала разбудить все ожидающие запросы
    mutex_lock(&pend_mtx);
    p = pend_head;
    while (p)
    {
        p->done = 1;
        p->res = -EINTR;
        wake_up_interruptible(&p->wait);
        p = p->next;
    }
    mutex_unlock(&pend_mtx);

    // Подождать завершения операций
    msleep(100);

    // Теперь освободить память
    mutex_lock(&pend_mtx);
    p = pend_head;
    while (p)
    {
        pending_t *n = p->next;
        if (p->data)
            kfree(p->data);
        kfree(p);
        p = n;
    }
    pend_head = NULL;
    mutex_unlock(&pend_mtx);

    if (nl_sock)
        netlink_kernel_release(nl_sock);
    if (inode_cache)
        kmem_cache_destroy(inode_cache);

    pr_info("btfs: Unloaded\n");
}

module_init(btfs_init);
module_exit(btfs_exit);

// SPDX-License-Identifier: GPL-2.0
/*
 * BTFS Client Filesystem - FIXED to match btfs_protocol.h
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/list.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BTFS Team");
MODULE_DESCRIPTION("Bluetooth Filesystem Client");

#define BTFS_MAGIC 0x42544653
#define NETLINK_BTFS 31
#define BTFS_MAX_PATH 256
#define BTFS_MAX_DATA 4096
#define BTFS_TIMEOUT (30*HZ)

#define BTFS_CHUNK_SIZE 2048  // Размер одного RPC запроса

/* Protocol - MUST MATCH btfs_protocol.h */
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
	BTFS_OP_TRUNCATE = 33,
};

/* Netlink wrapper */
struct btfs_nl_msg {
	u32 op;
	u32 seq;
	s32 res;
	u32 len;
	u8 data[0];
} __packed;

/* Protocol structures from btfs_protocol.h */
struct btfs_attr {
	u64 ino, size, blocks;
	u32 mode, nlink, uid, gid;
	u64 atime_sec, atime_nsec;
	u64 mtime_sec, mtime_nsec;
	u64 ctime_sec, ctime_nsec;
} __packed;

struct btfs_getattr_req {
	char path[BTFS_MAX_PATH];
} __packed;

struct btfs_readdir_req {
	char path[BTFS_MAX_PATH];
	u64 offset;
} __packed;

struct btfs_open_req {
	char path[BTFS_MAX_PATH];
	u32 flags;
	u32 mode;
	u32 lock_type;
} __packed;

struct btfs_open_resp {
	u64 file_handle;
	u32 lock_acquired;
} __packed;

struct btfs_read_req {
	u64 file_handle;
	u64 offset;
	u32 size;
} __packed;

struct btfs_read_resp {
	u32 bytes_read;
	char data[0];
} __packed;

struct btfs_write_req {
	u64 file_handle;
	u64 offset;
	u32 size;
	char data[0];
} __packed;

struct btfs_write_resp {
	u32 bytes_written;
} __packed;

struct btfs_close_req {
	u64 file_handle;
} __packed;

struct btfs_mkdir_req {
	char path[BTFS_MAX_PATH];
	u32 mode;
} __packed;

struct btfs_create_req {
	char path[BTFS_MAX_PATH];
	u32 mode;
	u32 flags;
} __packed;

struct btfs_unlink_req {
	char path[BTFS_MAX_PATH];
} __packed;

struct btfs_rename_req {
	char oldpath[BTFS_MAX_PATH];
	char newpath[BTFS_MAX_PATH];
} __packed;

struct btfs_truncate_req {
	char path[BTFS_MAX_PATH];
	u64 size;
} __packed;

struct btfs_dirent {
	u64 ino;
	u32 type;
	u32 name_len;
	char name[0];
} __packed;

/* RPC call context */
struct btfs_call {
	struct list_head link;
	u32 seq;
	wait_queue_head_t wq;
	int done;
	s32 err;
	void *buf;
	size_t buflen;
};

/* Mount context */
struct btfs_mnt {
	u32 daemon;
	int ready;
	atomic_t seqno;
	spinlock_t lock;
	struct list_head calls;
};

/* Inode context */
struct btfs_ino {
	struct inode vnode;
	u64 fh;
	struct mutex lock;
};

/* File context */
struct btfs_file {
	u64 fh;
};

static struct kmem_cache *btfs_ino_cache;
static struct sock *btfs_sock;
static struct btfs_mnt *btfs_mnt_active;
static DEFINE_SPINLOCK(btfs_mnt_lock);

#define BTFS_MNT(sb) ((struct btfs_mnt*)(sb)->s_fs_info)
#define BTFS_INO(i) container_of((i), struct btfs_ino, vnode)

/* Forward declarations */
static const struct inode_operations btfs_dir_iops;
static const struct inode_operations btfs_file_iops;
static const struct file_operations btfs_dir_fops;
static const struct file_operations btfs_file_fops;

/*
 * RPC helpers
 */
static struct btfs_call *btfs_call_new(struct btfs_mnt *mnt)
{
	struct btfs_call *call;
	
	call = kzalloc(sizeof(*call), GFP_NOFS);
	if (!call)
		return NULL;
	
	call->seq = atomic_inc_return(&mnt->seqno);
	init_waitqueue_head(&call->wq);
	
	spin_lock(&mnt->lock);
	list_add_tail(&call->link, &mnt->calls);
	spin_unlock(&mnt->lock);
	
	return call;
}

static void btfs_call_free(struct btfs_mnt *mnt, struct btfs_call *call)
{
	spin_lock(&mnt->lock);
	list_del(&call->link);
	spin_unlock(&mnt->lock);
	
	kfree(call->buf);
	kfree(call);
}

static struct btfs_call *btfs_call_find(struct btfs_mnt *mnt, u32 seq)
{
	struct btfs_call *call;
	
	spin_lock(&mnt->lock);
	list_for_each_entry(call, &mnt->calls, link) {
		if (call->seq == seq) {
			spin_unlock(&mnt->lock);
			return call;
		}
	}
	spin_unlock(&mnt->lock);
	return NULL;
}

static int btfs_rpc(struct btfs_mnt *mnt, u32 op,
		    const void *arg, size_t alen,
		    void *reply, size_t rlen)
{
	struct btfs_call *call;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct btfs_nl_msg *msg;
	struct sockaddr_nl dest;
	size_t tot;
	int ret;
	
	if (!mnt->ready)
		return -ENOTCONN;
	
	call = btfs_call_new(mnt);
	if (!call)
		return -ENOMEM;
	
	tot = sizeof(*msg) + alen;
	skb = nlmsg_new(tot, GFP_NOFS);
	if (!skb) {
		btfs_call_free(mnt, call);
		return -ENOMEM;
	}
	
	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, tot, 0);
	msg = nlmsg_data(nlh);
	msg->op = op;
	msg->seq = call->seq;
	msg->res = 0;
	msg->len = alen;
	if (arg && alen)
		memcpy(msg->data, arg, alen);
	
	memset(&dest, 0, sizeof(dest));
	dest.nl_family = AF_NETLINK;
	dest.nl_pid = mnt->daemon;
	
	NETLINK_CB(skb).dst_group = 0;
	
	ret = nlmsg_unicast(btfs_sock, skb, mnt->daemon);
	if (ret < 0) {
		btfs_call_free(mnt, call);
		return ret;
	}
	
	ret = wait_event_interruptible_timeout(call->wq, call->done, BTFS_TIMEOUT);
	if (ret == 0) {
		btfs_call_free(mnt, call);
		return -ETIMEDOUT;
	}
	if (ret < 0) {
		btfs_call_free(mnt, call);
		return ret;
	}
	
	ret = call->err;
	if (ret == 0 && reply && rlen && call->buf)
		memcpy(reply, call->buf, min(rlen, call->buflen));
	
	btfs_call_free(mnt, call);
	return ret;
}

/*
 * Netlink callback
 */
static void btfs_nl_input(struct sk_buff *skb)
{
    struct nlmsghdr *nlh = nlmsg_hdr(skb);
    struct btfs_nl_msg *msg = nlmsg_data(nlh);
    struct btfs_mnt *mnt;
    struct btfs_call *call;

    pr_info("btfs_nl_input: received nlmsg_len=%u\n", nlh->nlmsg_len);

    spin_lock(&btfs_mnt_lock);
    mnt = btfs_mnt_active;
    spin_unlock(&btfs_mnt_lock);

    if (!mnt) {
        pr_warn("btfs_nl_input: no active mount\n");
        return;
    }

    if (!mnt->ready) {
        mnt->daemon = nlh->nlmsg_pid;
        mnt->ready = 1;
        pr_info("btfs: daemon registered pid=%u\n", mnt->daemon);
        return;
    }

    call = btfs_call_find(mnt, msg->seq);
    if (!call) {
        pr_warn("btfs_nl_input: no call found for seq=%u\n", msg->seq);
        return;
    }

    pr_info("btfs_nl_input: seq=%u res=%d len=%u\n", msg->seq, msg->res, msg->len);

    call->err = msg->res;
    
    if (msg->len > 0 && msg->res >= 0) {
        pr_info("btfs_nl_input: trying to kmemdup %u bytes\n", msg->len);
        
        call->buf = kmemdup(msg->data, msg->len, GFP_ATOMIC);
        if (call->buf) {
            call->buflen = msg->len;
            pr_info("btfs_nl_input: allocated buffer %zu bytes\n", call->buflen);
        } else {
            pr_err("btfs_nl_input: kmemdup FAILED for %u bytes!\n", msg->len);
            call->err = -ENOMEM;
        }
    }

    call->done = 1;
    wake_up(&call->wq);
}


/*
 * Path conversion
 */
static int btfs_path(struct dentry *dentry, char *buf, size_t sz)
{
	char *p, *tmp = (char*)__get_free_page(GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	
	p = dentry_path_raw(dentry, tmp, PAGE_SIZE);
	if (IS_ERR(p)) {
		free_page((unsigned long)tmp);
		return PTR_ERR(p);
	}
	
	while (*p == '/') p++;
	strscpy(buf, *p ? p : ".", sz);
	free_page((unsigned long)tmp);
	return 0;
}

/*
 * Inode helpers
 */
static void btfs_fill_inode(struct inode *inode, struct btfs_attr *fa)
{
	inode->i_ino = fa->ino;
	inode->i_mode = fa->mode;
	set_nlink(inode, fa->nlink);
	i_uid_write(inode, fa->uid);
	i_gid_write(inode, fa->gid);
	i_size_write(inode, fa->size);
	inode->i_blocks = fa->blocks;
	
	inode_set_atime(inode, fa->atime_sec, fa->atime_nsec);
	inode_set_mtime(inode, fa->mtime_sec, fa->mtime_nsec);
	inode_set_ctime(inode, fa->ctime_sec, fa->ctime_nsec);
}

static struct inode *btfs_new_inode(struct super_block *sb, struct btfs_attr *fa)
{
	struct inode *inode = new_inode(sb);
	if (!inode)
		return NULL;
	
	btfs_fill_inode(inode, fa);
	
	if (S_ISDIR(fa->mode)) {
		inode->i_op = &btfs_dir_iops;
		inode->i_fop = &btfs_dir_fops;
	} else if (S_ISREG(fa->mode)) {
		inode->i_op = &btfs_file_iops;
		inode->i_fop = &btfs_file_fops;
		inode->i_data.a_ops = &empty_aops;
	}
	
	return inode;
}

/*
 * File operations
 */

 static int btfs_ensure_open(struct inode *inode, struct file *file)
{
	struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
	struct btfs_file *bf = file->private_data;
	struct btfs_open_req req;
	struct btfs_open_resp rsp;
	int ret;
	
	/* Уже открыт */
	if (bf && bf->fh != 0)
		return 0;
	
	pr_info("btfs: lazy open for inode %lu\n", inode->i_ino);
	
	/* Если bf не существует - создать */
	if (!bf) {
		bf = kzalloc(sizeof(*bf), GFP_KERNEL);
		if (!bf)
			return -ENOMEM;
		file->private_data = bf;
	}
	
	/* Вызвать OPEN RPC */
	memset(&req, 0, sizeof(req));
	ret = btfs_path(file->f_path.dentry, req.path, BTFS_MAX_PATH);
	if (ret) {
		kfree(bf);
		file->private_data = NULL;
		return ret;
	}
	
	/* Определить флаги из file->f_mode */
	req.flags = 0;
	if (file->f_mode & FMODE_READ)
		req.flags |= O_RDONLY;
	if (file->f_mode & FMODE_WRITE) {
		if (file->f_mode & FMODE_READ)
			req.flags = O_RDWR;
		else
			req.flags = O_WRONLY;
	}
	
	req.mode = 0;
	req.lock_type = 0;
	
	pr_info("btfs_ensure_open: path='%s' f_mode=0x%x req.flags=0x%x\n",
		req.path, file->f_mode, req.flags);
	
	ret = btfs_rpc(mnt, BTFS_OP_OPEN, &req, sizeof(req), &rsp, sizeof(rsp));
	if (ret < 0) {
		pr_err("btfs_ensure_open: RPC failed %d\n", ret);
		kfree(bf);
		file->private_data = NULL;
		return ret;
	}
	
	bf->fh = rsp.file_handle;
	
	pr_info("btfs_ensure_open: success handle=%llu\n", bf->fh);
	
	return 0;
}

static int btfs_open(struct inode *inode, struct file *file)
{
	/* Просто используем ensure_open */
	return btfs_ensure_open(inode, file);
}

static int btfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file_inode(file);
    struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
    struct btfs_file *bf = file->private_data;
    struct btfs_fsync_req {
        u64 file_handle;
    } __packed req;
    int ret;
    
    if (!bf || bf->fh == 0)
        return -EBADF;
    
    req.file_handle = bf->fh;
    
    pr_info("btfs_fsync: handle=%llu\n", bf->fh);
    
    ret = btfs_rpc(mnt, BTFS_OP_FSYNC, &req, sizeof(req), NULL, 0);
    if (ret < 0) {
        pr_err("btfs_fsync: RPC failed %d\n", ret);
        return ret;
    }
    
    pr_info("btfs_fsync: success\n");
    return 0;
}



static int btfs_release(struct inode *inode, struct file *file)
{
	struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
	struct btfs_file *bf = file->private_data;
	struct btfs_close_req req;
	
	if (!bf)
		return 0;
	
	pr_info("btfs_release: closing handle=%llu\n", bf->fh);
	
	req.file_handle = bf->fh;
	
	if (mnt->ready)
		btfs_rpc(mnt, BTFS_OP_CLOSE, &req, sizeof(req), NULL, 0);
	
	kfree(bf);
	file->private_data = NULL;
	
	return 0;
}


static ssize_t btfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(file);
    struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
    struct btfs_file *bf;
    struct btfs_read_req req;
    struct btfs_read_resp *rsp;
    char *rbuf;
    size_t count = iov_iter_count(to);
    loff_t pos = iocb->ki_pos;
    ssize_t total = 0;
    int ret;

    ret = btfs_ensure_open(inode, file);
    if (ret < 0)
        return ret;

    bf = file->private_data;
    if (!bf || bf->fh == 0)
        return -EBADF;

    rbuf = kmalloc(sizeof(*rsp) + BTFS_CHUNK_SIZE, GFP_KERNEL);
    if (!rbuf)
        return -ENOMEM;

    while (count > 0) {
        size_t chunk = min_t(size_t, count, BTFS_CHUNK_SIZE);
        
        req.file_handle = bf->fh;
        req.offset = pos;
        req.size = chunk;

        pr_info("btfs_read_iter: handle=%llu offset=%lld size=%zu\n",
                bf->fh, pos, chunk);

        ret = btfs_rpc(mnt, BTFS_OP_READ, &req, sizeof(req), rbuf, sizeof(*rsp) + chunk);
        if (ret < 0) {
            pr_err("btfs_read_iter: RPC failed %d\n", ret);
            kfree(rbuf);
            return total ? total : ret;
        }

        rsp = (struct btfs_read_resp *)rbuf;
        
        pr_info("btfs_read_iter: got bytes_read=%u\n", rsp->bytes_read);
        
        // EOF
        if (rsp->bytes_read == 0)
            break;

        if (copy_to_iter(rsp->data, rsp->bytes_read, to) != rsp->bytes_read) {
            kfree(rbuf);
            return total ? total : -EFAULT;
        }

        total += rsp->bytes_read;
        pos += rsp->bytes_read;
        count -= rsp->bytes_read;
    }

    kfree(rbuf);
    iocb->ki_pos = pos;
    return total;
}


static ssize_t btfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(file);
    struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
    struct btfs_file *bf;
    struct btfs_write_req *wreq;
    struct btfs_write_resp wrsp;
    size_t count = iov_iter_count(from);
    loff_t pos = iocb->ki_pos;
    size_t max_chunk = BTFS_CHUNK_SIZE;  // ИСПРАВЛЕНО: как в read
    ssize_t total = 0;
    int ret;

    ret = btfs_ensure_open(inode, file);
    if (ret < 0)
        return ret;

    bf = file->private_data;
    if (!bf || bf->fh == 0)
        return -EBADF;

    // ДОБАВЛЕНО: цикл для записи больших файлов
    while (count > 0) {
        size_t chunk = min_t(size_t, count, max_chunk);
        
        wreq = kmalloc(sizeof(*wreq) + chunk, GFP_KERNEL);
        if (!wreq)
            return total ? total : -ENOMEM;

        wreq->file_handle = bf->fh;
        wreq->offset = pos;
        wreq->size = chunk;

        if (!copy_from_iter_full(wreq->data, chunk, from)) {
            kfree(wreq);
            return total ? total : -EFAULT;
        }

        pr_info("btfs_write_iter: handle=%llu offset=%lld size=%zu\n",
                bf->fh, pos, chunk);

        ret = btfs_rpc(mnt, BTFS_OP_WRITE, wreq, sizeof(*wreq) + chunk, 
                       &wrsp, sizeof(wrsp));
        kfree(wreq);

        if (ret < 0) {
            pr_err("btfs_write_iter: RPC failed %d\n", ret);
            return total ? total : ret;
        }

        pr_info("btfs_write_iter: written %u bytes\n", wrsp.bytes_written);

        // Если записано меньше chunk - ошибка или диск полон
        if (wrsp.bytes_written < chunk) {
            pr_warn("btfs_write_iter: short write (%u < %zu)\n", 
                    wrsp.bytes_written, chunk);
            total += wrsp.bytes_written;
            pos += wrsp.bytes_written;
            break;
        }

        total += wrsp.bytes_written;
        pos += wrsp.bytes_written;
        count -= wrsp.bytes_written;
    }

    iocb->ki_pos = pos;
    if (iocb->ki_pos > i_size_read(inode))
        i_size_write(inode, iocb->ki_pos);

    return total;
}



static int btfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
	struct btfs_readdir_req req;
	char *buf;
	size_t off;
	int ret;
	
	if (ctx->pos == 0) {
		if (!dir_emit_dot(file, ctx))
			return 0;
		ctx->pos = 1;
	}
	
	if (ctx->pos == 1) {
		if (!dir_emit_dotdot(file, ctx))
			return 0;
		ctx->pos = 2;
	}
	
	if (ctx->pos == 2) {
		memset(&req, 0, sizeof(req));
		ret = btfs_path(file->f_path.dentry, req.path, BTFS_MAX_PATH);
		if (ret)
			return ret;
		
		req.offset = 0;
		
		buf = kmalloc(4096, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		
		ret = btfs_rpc(mnt, BTFS_OP_READDIR, &req, sizeof(req), buf, 4096);
		if (ret < 0) {
			kfree(buf);
			return ret;
		}
		
		off = 0;
		while (off + sizeof(struct btfs_dirent) <= 4096) {
			struct btfs_dirent *de = (void*)(buf + off);
			size_t entry_size;
			
			/* Конец списка */
			if (de->name_len == 0 || de->name_len > 255)
				break;
			
			/* Проверка что запись полностью влезает */
			entry_size = sizeof(struct btfs_dirent) + de->name_len + 1;
			if (off + entry_size > 4096)
				break;
			
			/* Убедиться что имя null-terminated */
			de->name[de->name_len] = '\0';
			
			pr_info("btfs_readdir: emit ino=%llu type=%u name='%s'\n",
				de->ino, de->type, de->name);
			
			if (!dir_emit(ctx, de->name, de->name_len, de->ino, de->type))
				break;
			
			/* ИСПРАВЛЕНО: сервер НЕ выравнивает, просто переходим к следующей записи */
			off += entry_size;
		}
		
		kfree(buf);
		ctx->pos = 3;
	}
	
	return 0;
}



static const struct file_operations btfs_file_fops = {
	.open = btfs_open,
	.release = btfs_release,
	.read_iter = btfs_read_iter,
	.write_iter = btfs_write_iter,
	.fsync = btfs_fsync,
	.llseek = generic_file_llseek,
};

static const struct file_operations btfs_dir_fops = {
	.iterate_shared = btfs_readdir,
	.llseek = generic_file_llseek,
};

/*
 * Inode operations
 */
static int btfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
	struct btfs_getattr_req req;
	struct btfs_attr fa;
	int ret;
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(path->dentry, req.path, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	ret = btfs_rpc(mnt, BTFS_OP_GETATTR, &req, sizeof(req), &fa, sizeof(fa));
	if (ret)
		return ret;
	
	btfs_fill_inode(inode, &fa);
	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static struct dentry *btfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct btfs_mnt *mnt = BTFS_MNT(dir->i_sb);
	struct btfs_getattr_req req;
	struct btfs_attr fa;
	struct inode *inode;
	int ret;
	
	if (dentry->d_name.len > 255)
		return ERR_PTR(-ENAMETOOLONG);
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(dentry, req.path, BTFS_MAX_PATH);
	if (ret)
		return ERR_PTR(ret);
	
	ret = btfs_rpc(mnt, BTFS_OP_GETATTR, &req, sizeof(req), &fa, sizeof(fa));
	if (ret == -ENOENT)
		return d_splice_alias(NULL, dentry);
	if (ret)
		return ERR_PTR(ret);
	
	inode = btfs_new_inode(dir->i_sb, &fa);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	
	return d_splice_alias(inode, dentry);
}

static int btfs_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	struct btfs_mnt *mnt = BTFS_MNT(dir->i_sb);
	struct btfs_create_req req;
	struct inode *inode;
	int ret;
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(dentry, req.path, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	req.mode = mode;
	req.flags = 0;
	
	ret = btfs_rpc(mnt, BTFS_OP_CREATE, &req, sizeof(req), NULL, 0);
	if (ret)
		return ret;
	
	inode = new_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;
	
	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_op = &btfs_file_iops;
	inode->i_fop = &btfs_file_fops;
	inode->i_data.a_ops = &empty_aops;
	inode_init_owner(idmap, inode, dir, mode);
	
	d_instantiate(dentry, inode);
	return 0;
}

static int btfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct btfs_mnt *mnt = BTFS_MNT(dir->i_sb);
	struct btfs_unlink_req req;
	int ret;
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(dentry, req.path, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	return btfs_rpc(mnt, BTFS_OP_UNLINK, &req, sizeof(req), NULL, 0);
}

static int btfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	struct btfs_mnt *mnt = BTFS_MNT(dir->i_sb);
	struct btfs_mkdir_req req;
	struct inode *inode;
	int ret;
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(dentry, req.path, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	req.mode = mode;
	
	ret = btfs_rpc(mnt, BTFS_OP_MKDIR, &req, sizeof(req), NULL, 0);
	if (ret)
		return ret;
	
	inode = new_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;
	
	inode->i_ino = get_next_ino();
	inode->i_mode = S_IFDIR | mode;
	inode->i_op = &btfs_dir_iops;
	inode->i_fop = &btfs_dir_fops;
	inode_init_owner(idmap, inode, dir, S_IFDIR | mode);
	set_nlink(inode, 2);
	
	d_instantiate(dentry, inode);
	inc_nlink(dir);
	return 0;
}

static int btfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct btfs_mnt *mnt = BTFS_MNT(dir->i_sb);
	struct btfs_unlink_req req;
	int ret;
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(dentry, req.path, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	ret = btfs_rpc(mnt, BTFS_OP_RMDIR, &req, sizeof(req), NULL, 0);
	if (ret == 0)
		drop_nlink(dir);
	
	return ret;
}

static int btfs_rename(struct mnt_idmap *idmap,
		       struct inode *odir, struct dentry *odentry,
		       struct inode *ndir, struct dentry *ndentry,
		       unsigned int flags)
{
	struct btfs_mnt *mnt = BTFS_MNT(odir->i_sb);
	struct btfs_rename_req req;
	int ret;
	
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	
	memset(&req, 0, sizeof(req));
	ret = btfs_path(odentry, req.oldpath, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	ret = btfs_path(ndentry, req.newpath, BTFS_MAX_PATH);
	if (ret)
		return ret;
	
	return btfs_rpc(mnt, BTFS_OP_RENAME, &req, sizeof(req), NULL, 0);
}

static int btfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct btfs_mnt *mnt = BTFS_MNT(inode->i_sb);
	struct btfs_truncate_req req;
	int ret;
	
	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;
	
	if (attr->ia_valid & ATTR_SIZE) {
		memset(&req, 0, sizeof(req));
		ret = btfs_path(dentry, req.path, BTFS_MAX_PATH);
		if (ret)
			return ret;
		
		req.size = attr->ia_size;
		
		ret = btfs_rpc(mnt, BTFS_OP_TRUNCATE, &req, sizeof(req), NULL, 0);
		if (ret)
			return ret;
		
		truncate_setsize(inode, attr->ia_size);
	}
	
	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

static const struct inode_operations btfs_file_iops = {
	.getattr = btfs_getattr,
	.setattr = btfs_setattr,
};

static const struct inode_operations btfs_dir_iops = {
	.lookup = btfs_lookup,
	.getattr = btfs_getattr,
	.create = btfs_create,
	.unlink = btfs_unlink,
	.mkdir = btfs_mkdir,
	.rmdir = btfs_rmdir,
	.rename = btfs_rename,
};

/*
 * Superblock operations
 */
static struct inode *btfs_alloc_inode(struct super_block *sb)
{
	struct btfs_ino *bi = kmem_cache_alloc(btfs_ino_cache, GFP_KERNEL);
	if (!bi)
		return NULL;
	
	bi->fh = 0;
	mutex_init(&bi->lock);
	return &bi->vnode;
}

static void btfs_free_inode(struct inode *inode)
{
	kmem_cache_free(btfs_ino_cache, BTFS_INO(inode));
}

static void btfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void btfs_put_super(struct super_block *sb)
{
	struct btfs_mnt *mnt = BTFS_MNT(sb);
	struct btfs_call *call, *tmp;
	
	if (!mnt)
		return;
	
	spin_lock(&btfs_mnt_lock);
	if (btfs_mnt_active == mnt)
		btfs_mnt_active = NULL;
	spin_unlock(&btfs_mnt_lock);
	
	spin_lock(&mnt->lock);
	list_for_each_entry_safe(call, tmp, &mnt->calls, link) {
		call->done = 1;
		call->err = -ESHUTDOWN;
		wake_up(&call->wq);
	}
	spin_unlock(&mnt->lock);
	
	kfree(mnt);
}

static const struct super_operations btfs_sops = {
	.alloc_inode = btfs_alloc_inode,
	.free_inode = btfs_free_inode,
	.evict_inode = btfs_evict_inode,
	.put_super = btfs_put_super,
	.statfs = simple_statfs,
	.drop_inode = generic_delete_inode,
};

static int btfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct btfs_mnt *mnt;
	struct inode *root;
	
	mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
	if (!mnt)
		return -ENOMEM;
	
	spin_lock_init(&mnt->lock);
	INIT_LIST_HEAD(&mnt->calls);
	atomic_set(&mnt->seqno, 0);
	
	spin_lock(&btfs_mnt_lock);
	btfs_mnt_active = mnt;
	spin_unlock(&btfs_mnt_lock);
	
	sb->s_fs_info = mnt;
	sb->s_magic = BTFS_MAGIC;
	sb->s_op = &btfs_sops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_time_gran = 1;
	
	root = new_inode(sb);
	if (!root)
		return -ENOMEM;
	
	root->i_ino = 1;
	root->i_mode = S_IFDIR | 0755;
	root->i_op = &btfs_dir_iops;
	root->i_fop = &btfs_dir_fops;
	set_nlink(root, 2);
	inode_init_owner(&nop_mnt_idmap, root, NULL, S_IFDIR | 0755);
	
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;
	
	return 0;
}

static struct dentry *btfs_mount(struct file_system_type *fs, int flags,
				 const char *dev, void *data)
{
	return mount_nodev(fs, flags, data, btfs_fill_super);
}

static struct file_system_type btfs_type = {
	.owner = THIS_MODULE,
	.name = "btfs",
	.mount = btfs_mount,
	.kill_sb = kill_litter_super,
};

static void btfs_ino_init(void *obj)
{
	struct btfs_ino *bi = obj;
	inode_init_once(&bi->vnode);
}

static int __init btfs_init(void)
{
	struct netlink_kernel_cfg cfg = { .input = btfs_nl_input };
	
	btfs_ino_cache = kmem_cache_create("btfs_inode",
					   sizeof(struct btfs_ino), 0,
					   SLAB_RECLAIM_ACCOUNT|SLAB_ACCOUNT,
					   btfs_ino_init);
	if (!btfs_ino_cache)
		return -ENOMEM;
	
	btfs_sock = netlink_kernel_create(&init_net, NETLINK_BTFS, &cfg);
	if (!btfs_sock) {
		kmem_cache_destroy(btfs_ino_cache);
		return -ENOMEM;
	}
	
	if (register_filesystem(&btfs_type)) {
		netlink_kernel_release(btfs_sock);
		kmem_cache_destroy(btfs_ino_cache);
		return -EBUSY;
	}
	
	pr_info("btfs: module loaded\n");
	return 0;
}

static void __exit btfs_exit(void)
{
	unregister_filesystem(&btfs_type);
	netlink_kernel_release(btfs_sock);
	rcu_barrier();
	kmem_cache_destroy(btfs_ino_cache);
	pr_info("btfs: module unloaded\n");
}

module_init(btfs_init);
module_exit(btfs_exit);

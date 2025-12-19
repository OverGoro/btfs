// btfs_client_fs.c - Professional implementation based on NFS/CIFS architecture for Linux 6.14
// SPDX-License-Identifier: GPL-2.0

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
#include <linux/backing-dev.h>
#include <linux/namei.h>
#include <linux/xattr.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BTFS Team");
MODULE_DESCRIPTION("Bluetooth Distributed File System - Client");
MODULE_VERSION("1.0");

#define BTFS_MAGIC 0x42544653
#define NETLINK_BTFS 31
#define BTFS_MAX_PATH 256
#define BTFS_MAX_DATA 4096
#define BTFS_DEFAULT_TIMEOUT (30 * HZ)

// Protocol opcodes (from btfs_protocol.h)
enum btfs_opcode {
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

// Lock types
enum btfs_lock_type {
	BTFS_LOCK_NONE = 0,
	BTFS_LOCK_READ = 1,
	BTFS_LOCK_WRITE = 2,
};

// Netlink message structure
struct btfs_nl_msg {
	u32 opcode;
	u32 sequence;
	s32 result;
	u32 data_len;
	u8 data[];
} __packed;

// Attribute structure
struct btfs_attr {
	u64 ino;
	u64 size;
	u64 blocks;
	u32 mode;
	u32 nlink;
	u32 uid;
	u32 gid;
	u64 atime_sec;
	u64 atime_nsec;
	u64 mtime_sec;
	u64 mtime_nsec;
	u64 ctime_sec;
	u64 ctime_nsec;
} __packed;

// Request structures
struct btfs_path_req {
	char path[BTFS_MAX_PATH];
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

struct btfs_rw_req {
	u64 file_handle;
	u64 offset;
	u32 size;
} __packed;

struct btfs_read_resp {
	u32 bytes_read;
	char data[];
} __packed;

struct btfs_write_req {
	u64 file_handle;
	u64 offset;
	u32 size;
	char data[];
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

struct btfs_rename_req {
	char oldpath[BTFS_MAX_PATH];
	char newpath[BTFS_MAX_PATH];
} __packed;

struct btfs_truncate_req {
	char path[BTFS_MAX_PATH];
	u64 size;
} __packed;

struct btfs_readdir_req {
	char path[BTFS_MAX_PATH];
	u64 offset;
} __packed;

struct btfs_dirent {
	u64 ino;
	u32 type;
	u32 name_len;
	char name[];
} __packed;

// Forward declarations
static const struct inode_operations btfs_file_inode_operations;
static const struct inode_operations btfs_dir_inode_operations;
static const struct file_operations btfs_file_operations;
static const struct file_operations btfs_dir_operations;

// RPC request tracking (similar to NFS rpc_task)
struct btfs_rpc_req {
	struct list_head list;
	u32 seq;
	wait_queue_head_t wait;
	bool completed;
	s32 result;
	void *reply;
	size_t reply_len;
	struct rcu_head rcu;
};

// Per-inode information (similar to nfs_inode)
struct btfs_inode_info {
	u64 file_handle;
	unsigned long flags;
	struct mutex open_lock;
	struct inode vfs_inode;
};

// Per-file context (similar to nfs_open_context)
struct btfs_file_info {
	u64 handle;
	int flags;
	bool handle_valid;
};

// Superblock context (similar to nfs_server)
struct btfs_sb_info {
	u32 daemon_pid;
	spinlock_t req_lock;
	struct list_head pending_reqs;
	atomic_t seq_counter;
	bool daemon_alive;
};

// Global state
static struct kmem_cache *btfs_inode_cachep;
static struct sock *btfs_nl_sock;
static DEFINE_SPINLOCK(btfs_sb_lock);
static struct btfs_sb_info *btfs_current_sbi;

// Inode cache constructor
static void btfs_inode_init_once(void *foo)
{
	struct btfs_inode_info *info = foo;
	
	info->file_handle = 0;
	info->flags = 0;
	mutex_init(&info->open_lock);
	inode_init_once(&info->vfs_inode);
}

// Helper macros
#define BTFS_I(inode) container_of(inode, struct btfs_inode_info, vfs_inode)
#define BTFS_SB(sb) ((struct btfs_sb_info *)(sb)->s_fs_info)

// ============================================================================
// RPC Request Management (similar to NFS RPC layer)
// ============================================================================

static struct btfs_rpc_req *btfs_rpc_alloc(struct btfs_sb_info *sbi)
{
	struct btfs_rpc_req *req;
	
	req = kzalloc(sizeof(*req), GFP_NOFS);
	if (!req)
		return NULL;
	
	req->seq = atomic_inc_return(&sbi->seq_counter);
	init_waitqueue_head(&req->wait);
	req->completed = false;
	
	spin_lock(&sbi->req_lock);
	list_add_tail_rcu(&req->list, &sbi->pending_reqs);
	spin_unlock(&sbi->req_lock);
	
	return req;
}

static void btfs_rpc_free_callback(struct rcu_head *head)
{
	struct btfs_rpc_req *req = container_of(head, struct btfs_rpc_req, rcu);
	kfree(req->reply);
	kfree(req);
}

static void btfs_rpc_free(struct btfs_sb_info *sbi, struct btfs_rpc_req *req)
{
	spin_lock(&sbi->req_lock);
	list_del_rcu(&req->list);
	spin_unlock(&sbi->req_lock);
	
	call_rcu(&req->rcu, btfs_rpc_free_callback);
}

static struct btfs_rpc_req *btfs_rpc_find(struct btfs_sb_info *sbi, u32 seq)
{
	struct btfs_rpc_req *req;
	
	rcu_read_lock();
	list_for_each_entry_rcu(req, &sbi->pending_reqs, list) {
		if (req->seq == seq) {
			rcu_read_unlock();
			return req;
		}
	}
	rcu_read_unlock();
	
	return NULL;
}

static int btfs_rpc_send(struct btfs_sb_info *sbi, u32 opcode,
			 const void *data, size_t len, u32 *seq_out)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct btfs_nl_msg *msg;
	size_t total_len;
	int ret;
	
	if (!sbi->daemon_alive)
		return -ENOTCONN;
	
	*seq_out = atomic_read(&sbi->seq_counter) + 1;
	
	total_len = sizeof(struct btfs_nl_msg) + len;
	skb = nlmsg_new(total_len, GFP_NOFS);
	if (!skb)
		return -ENOMEM;
	
	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, total_len, 0);
	if (!nlh) {
		nlmsg_free(skb);
		return -ENOMEM;
	}
	
	msg = nlmsg_data(nlh);
	msg->opcode = opcode;
	msg->sequence = *seq_out;
	msg->result = 0;
	msg->data_len = len;
	
	if (data && len)
		memcpy(msg->data, data, len);
	
	NETLINK_CB(skb).dst_group = 0;
	
	ret = nlmsg_unicast(btfs_nl_sock, skb, sbi->daemon_pid);
	if (ret < 0) {
		sbi->daemon_alive = false;
		return -ENOTCONN;
	}
	
	return 0;
}

static int btfs_rpc_wait_reply(struct btfs_rpc_req *req, void *reply_buf, size_t buf_size)
{
	long timeout;
	int ret;
	
	timeout = wait_event_interruptible_timeout(req->wait,
						   req->completed,
						   BTFS_DEFAULT_TIMEOUT);
	
	if (timeout == 0)
		return -ETIMEDOUT;
	if (timeout < 0)
		return timeout;
	
	ret = req->result;
	
	if (ret >= 0 && reply_buf && buf_size && req->reply && req->reply_len) {
		size_t copy_len = min(buf_size, req->reply_len);
		memcpy(reply_buf, req->reply, copy_len);
	}
	
	return ret;
}

// Netlink receive handler
static void btfs_nl_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	struct btfs_nl_msg *msg;
	struct btfs_rpc_req *req;
	struct btfs_sb_info *sbi;
	
	nlh = nlmsg_hdr(skb);
	msg = nlmsg_data(nlh);
	
	// ИСПРАВЛЕНО: используем глобальную переменную
	spin_lock(&btfs_sb_lock);
	sbi = btfs_current_sbi;
	spin_unlock(&btfs_sb_lock);
	
	if (!sbi)
		return;
	
	if (!sbi->daemon_pid) {
		sbi->daemon_pid = nlh->nlmsg_pid;
		sbi->daemon_alive = true;
		pr_info("btfs: Daemon connected, PID=%u\n", sbi->daemon_pid);
		return;
	}
	
	req = btfs_rpc_find(sbi, msg->sequence);
	if (!req)
		return;
	
	req->result = msg->result;
	
	if (msg->data_len > 0) {
		req->reply = kmemdup(msg->data, msg->data_len, GFP_ATOMIC);
		if (req->reply)
			req->reply_len = msg->data_len;
	}
	
	req->completed = true;
	wake_up(&req->wait);
}

// ============================================================================
// Path Helpers
// ============================================================================

static void btfs_get_path(struct dentry *dentry, char *buf, size_t size)
{
	char *path, *tmp;
	
	tmp = __getname();
	if (!tmp) {
		strscpy(buf, ".", size);
		return;
	}
	
	path = dentry_path_raw(dentry, tmp, PATH_MAX);
	if (IS_ERR(path)) {
		strscpy(buf, ".", size);
	} else {
		// Skip leading slashes
		while (*path == '/')
			path++;
		strscpy(buf, *path ? path : ".", size);
	}
	
	__putname(tmp);
}

// ============================================================================
// Inode Attribute Management (like nfs_refresh_inode)
// ============================================================================

static void btfs_refresh_inode(struct inode *inode, const struct btfs_attr *attr)
{
	inode->i_ino = attr->ino;
	inode->i_mode = attr->mode;
	set_nlink(inode, attr->nlink);
	i_uid_write(inode, attr->uid);
	i_gid_write(inode, attr->gid);
	i_size_write(inode, attr->size);
	inode->i_blocks = attr->blocks;
	
	// Time handling for Linux 6.14
	inode_set_atime(inode, attr->atime_sec, attr->atime_nsec);
	inode_set_mtime(inode, attr->mtime_sec, attr->mtime_nsec);
	inode_set_ctime(inode, attr->ctime_sec, attr->ctime_nsec);
}

// ============================================================================
// File Operations (like NFS file_operations)
// ============================================================================

static int btfs_file_open(struct inode *inode, struct file *file)
{
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_inode_info *info = BTFS_I(inode);
	struct btfs_file_info *finfo;
	struct btfs_rpc_req *req;
	struct btfs_open_req open_req;
	struct btfs_open_resp open_resp;
	u32 seq;
	int ret;
	
	finfo = kzalloc(sizeof(*finfo), GFP_KERNEL);
	if (!finfo)
		return -ENOMEM;
	
	mutex_lock(&info->open_lock);
	
	// Reuse handle if already open
	if (info->file_handle) {
		finfo->handle = info->file_handle;
		finfo->handle_valid = true;
		finfo->flags = file->f_flags;
		file->private_data = finfo;
		mutex_unlock(&info->open_lock);
		return 0;
	}
	
	btfs_get_path(file->f_path.dentry, open_req.path, BTFS_MAX_PATH);
	open_req.flags = file->f_flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_TRUNC);
	open_req.mode = 0;
	open_req.lock_type = BTFS_LOCK_NONE;
	
	req = btfs_rpc_alloc(sbi);
	if (!req) {
		mutex_unlock(&info->open_lock);
		kfree(finfo);
		return -ENOMEM;
	}
	
	ret = btfs_rpc_send(sbi, BTFS_OP_OPEN, &open_req, sizeof(open_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		mutex_unlock(&info->open_lock);
		kfree(finfo);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, &open_resp, sizeof(open_resp));
	btfs_rpc_free(sbi, req);
	
	if (ret < 0) {
		mutex_unlock(&info->open_lock);
		kfree(finfo);
		return ret;
	}
	
	info->file_handle = open_resp.file_handle;
	finfo->handle = open_resp.file_handle;
	finfo->handle_valid = true;
	finfo->flags = file->f_flags;
	file->private_data = finfo;
	
	mutex_unlock(&info->open_lock);
	return 0;
}

static int btfs_file_release(struct inode *inode, struct file *file)
{
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_file_info *finfo = file->private_data;
	struct btfs_rpc_req *req;
	struct btfs_close_req close_req;
	u32 seq;
	
	if (!finfo)
		return 0;
	
	if (!finfo->handle_valid || !sbi->daemon_alive) {
		kfree(finfo);
		return 0;
	}
	
	close_req.file_handle = finfo->handle;
	
	req = btfs_rpc_alloc(sbi);
	if (req) {
		if (btfs_rpc_send(sbi, BTFS_OP_CLOSE, &close_req,
				  sizeof(u64), &seq) == 0) {
			btfs_rpc_wait_reply(req, NULL, 0);
		}
		btfs_rpc_free(sbi, req);
	}
	
	kfree(finfo);
	return 0;
}

static ssize_t btfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_file_info *finfo = file->private_data;
	struct btfs_rpc_req *req;
	struct btfs_rw_req read_req;
	struct btfs_read_resp *read_resp;
	char *rbuf;
	u32 seq;
	ssize_t ret;
	size_t count = iov_iter_count(to);
	loff_t pos = iocb->ki_pos;
	ssize_t total = 0;
	
	if (!finfo || !finfo->handle_valid)
		return -EBADF;
	
	while (count > 0) {
		size_t chunk = min_t(size_t, count, PAGE_SIZE);
		
		read_req.file_handle = finfo->handle;
		read_req.offset = pos;
		read_req.size = chunk;
		
		req = btfs_rpc_alloc(sbi);
		if (!req)
			return total ? total : -ENOMEM;
		
		ret = btfs_rpc_send(sbi, BTFS_OP_READ, &read_req, sizeof(read_req), &seq);
		if (ret < 0) {
			btfs_rpc_free(sbi, req);
			return total ? total : ret;
		}
		
		rbuf = kmalloc(sizeof(struct btfs_read_resp) + chunk, GFP_NOFS);
		if (!rbuf) {
			btfs_rpc_free(sbi, req);
			return total ? total : -ENOMEM;
		}
		
		ret = btfs_rpc_wait_reply(req, rbuf, sizeof(struct btfs_read_resp) + chunk);
		btfs_rpc_free(sbi, req);
		
		if (ret < 0) {
			kfree(rbuf);
			return total ? total : ret;
		}
		
		read_resp = (struct btfs_read_resp *)rbuf;
		
		if (read_resp->bytes_read > 0) {
			size_t copied = copy_to_iter(read_resp->data,
						     read_resp->bytes_read, to);
			if (copied != read_resp->bytes_read) {
				kfree(rbuf);
				return total ? total : -EFAULT;
			}
			
			total += copied;
			pos += copied;
			count -= copied;
			
			if (read_resp->bytes_read < chunk) {
				kfree(rbuf);
				break;
			}
		} else {
			kfree(rbuf);
			break;
		}
		
		kfree(rbuf);
	}
	
	iocb->ki_pos = pos;
	return total;
}

static ssize_t btfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_file_info *finfo = file->private_data;
	struct btfs_rpc_req *req;
	struct btfs_write_req *write_req;
	struct btfs_write_resp write_resp;
	size_t total_size;
	u32 seq;
	int ret;
	size_t count = iov_iter_count(from);
	loff_t pos = iocb->ki_pos;
	
	if (!finfo || !finfo->handle_valid)
		return -EBADF;
	
	total_size = sizeof(struct btfs_write_req) + count;
	write_req = kmalloc(total_size, GFP_NOFS);
	if (!write_req)
		return -ENOMEM;
	
	write_req->file_handle = finfo->handle;
	write_req->offset = pos;
	write_req->size = count;
	
	if (!copy_from_iter_full(write_req->data, count, from)) {
		kfree(write_req);
		return -EFAULT;
	}
	
	req = btfs_rpc_alloc(sbi);
	if (!req) {
		kfree(write_req);
		return -ENOMEM;
	}
	
	ret = btfs_rpc_send(sbi, BTFS_OP_WRITE, write_req, total_size, &seq);
	kfree(write_req);
	
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, &write_resp, sizeof(write_resp));
	btfs_rpc_free(sbi, req);
	
	if (ret < 0)
		return ret;
	
	iocb->ki_pos = pos + write_resp.bytes_written;
	
	if (iocb->ki_pos > i_size_read(inode))
		i_size_write(inode, iocb->ki_pos);
	
	return write_resp.bytes_written;
}

static int btfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_readdir_req readdir_req;
	char *rbuf;
	u32 seq;
	int ret;
	size_t offset;
	
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
	
	if (ctx->pos >= 2)
		return 0;
	
	btfs_get_path(file->f_path.dentry, readdir_req.path, BTFS_MAX_PATH);
	readdir_req.offset = 0;
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_READDIR, &readdir_req, sizeof(readdir_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	rbuf = kmalloc(BTFS_MAX_DATA, GFP_NOFS);
	if (!rbuf) {
		btfs_rpc_free(sbi, req);
		return -ENOMEM;
	}
	
	ret = btfs_rpc_wait_reply(req, rbuf, BTFS_MAX_DATA);
	btfs_rpc_free(sbi, req);
	
	if (ret < 0) {
		kfree(rbuf);
		return ret;
	}
	
	offset = 0;
	while (offset < BTFS_MAX_DATA) {
		struct btfs_dirent *dirent = (struct btfs_dirent *)(rbuf + offset);
		
		if (!dirent->name_len || dirent->name_len > NAME_MAX)
			break;
		
		if (!dir_emit(ctx, dirent->name, dirent->name_len,
			      dirent->ino, dirent->type))
			break;
		
		offset += sizeof(struct btfs_dirent) + dirent->name_len + 1;
	}
	
	kfree(rbuf);
	ctx->pos = 3;
	
	return 0;
}

static const struct file_operations btfs_file_operations = {
	.open		= btfs_file_open,
	.release	= btfs_file_release,
	.read_iter	= btfs_file_read_iter,
	.write_iter	= btfs_file_write_iter,
	.llseek		= generic_file_llseek,
	.fsync		= noop_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
};

static const struct file_operations btfs_dir_operations = {
	.iterate_shared	= btfs_readdir,
	.llseek		= generic_file_llseek,
};

// ============================================================================
// Inode Operations (like NFS inode_operations)
// ============================================================================

static int btfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_path_req getattr_req;
	struct btfs_attr attr;
	u32 seq;
	int ret;
	
	btfs_get_path(path->dentry, getattr_req.path, BTFS_MAX_PATH);
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_GETATTR, &getattr_req, sizeof(getattr_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, &attr, sizeof(attr));
	btfs_rpc_free(sbi, req);
	
	if (ret < 0)
		return ret;
	
	btfs_refresh_inode(inode, &attr);
	generic_fillattr(idmap, request_mask, inode, stat);
	
	return 0;
}

static struct dentry *btfs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct btfs_sb_info *sbi = BTFS_SB(dir->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_path_req getattr_req;
	struct btfs_attr attr;
	struct inode *inode = NULL;
	u32 seq;
	int ret;
	
	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	
	btfs_get_path(dentry, getattr_req.path, BTFS_MAX_PATH);
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return ERR_PTR(-ENOMEM);
	
	ret = btfs_rpc_send(sbi, BTFS_OP_GETATTR, &getattr_req, sizeof(getattr_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ERR_PTR(ret);
	}
	
	ret = btfs_rpc_wait_reply(req, &attr, sizeof(attr));
	btfs_rpc_free(sbi, req);
	
	if (ret == -ENOENT)
		return d_splice_alias(NULL, dentry);
	
	if (ret < 0)
		return ERR_PTR(ret);
	
	inode = new_inode(dir->i_sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	
	btfs_refresh_inode(inode, &attr);
	
	if (S_ISDIR(attr.mode)) {
		inode->i_op = &btfs_dir_inode_operations;
		inode->i_fop = &btfs_dir_operations;
	} else if (S_ISREG(attr.mode)) {
		inode->i_op = &btfs_file_inode_operations;
		inode->i_fop = &btfs_file_operations;
		inode->i_data.a_ops = &empty_aops;
	} else {
		init_special_inode(inode, attr.mode, 0);
	}
	
	unlock_new_inode(inode);
	return d_splice_alias(inode, dentry);
}

static int btfs_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	struct btfs_sb_info *sbi = BTFS_SB(dir->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_create_req create_req;
	struct inode *inode;
	u32 seq;
	int ret;
	
	btfs_get_path(dentry, create_req.path, BTFS_MAX_PATH);
	create_req.mode = mode;
	create_req.flags = 0;
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_CREATE, &create_req, sizeof(create_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, NULL, 0);
	btfs_rpc_free(sbi, req);
	
	if (ret < 0)
		return ret;
	
	inode = new_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;
	
	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_op = &btfs_file_inode_operations;
	inode->i_fop = &btfs_file_operations;
	inode->i_data.a_ops = &empty_aops;
	inode_init_owner(idmap, inode, dir, mode);
	
	d_instantiate(dentry, inode);
	return 0;
}

static int btfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct btfs_sb_info *sbi = BTFS_SB(dir->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_path_req unlink_req;
	u32 seq;
	int ret;
	
	btfs_get_path(dentry, unlink_req.path, BTFS_MAX_PATH);
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_UNLINK, &unlink_req, sizeof(unlink_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, NULL, 0);
	btfs_rpc_free(sbi, req);
	
	return ret;
}

static int btfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	struct btfs_sb_info *sbi = BTFS_SB(dir->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_mkdir_req mkdir_req;
	struct inode *inode;
	u32 seq;
	int ret;
	
	btfs_get_path(dentry, mkdir_req.path, BTFS_MAX_PATH);
	mkdir_req.mode = mode;
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_MKDIR, &mkdir_req, sizeof(mkdir_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, NULL, 0);
	btfs_rpc_free(sbi, req);
	
	if (ret < 0)
		return ret;
	
	inode = new_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;
	
	inode->i_ino = get_next_ino();
	inode->i_mode = S_IFDIR | mode;
	inode->i_op = &btfs_dir_inode_operations;
	inode->i_fop = &btfs_dir_operations;
	inode_init_owner(idmap, inode, dir, S_IFDIR | mode);
	set_nlink(inode, 2);
	
	d_instantiate(dentry, inode);
	inc_nlink(dir);
	
	return 0;
}

static int btfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct btfs_sb_info *sbi = BTFS_SB(dir->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_path_req rmdir_req;
	u32 seq;
	int ret;
	
	btfs_get_path(dentry, rmdir_req.path, BTFS_MAX_PATH);
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_RMDIR, &rmdir_req, sizeof(rmdir_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, NULL, 0);
	btfs_rpc_free(sbi, req);
	
	if (ret == 0)
		drop_nlink(dir);
	
	return ret;
}

static int btfs_rename(struct mnt_idmap *idmap,
		       struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry,
		       unsigned int flags)
{
	struct btfs_sb_info *sbi = BTFS_SB(old_dir->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_rename_req rename_req;
	u32 seq;
	int ret;
	
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	
	btfs_get_path(old_dentry, rename_req.oldpath, BTFS_MAX_PATH);
	btfs_get_path(new_dentry, rename_req.newpath, BTFS_MAX_PATH);
	
	req = btfs_rpc_alloc(sbi);
	if (!req)
		return -ENOMEM;
	
	ret = btfs_rpc_send(sbi, BTFS_OP_RENAME, &rename_req, sizeof(rename_req), &seq);
	if (ret < 0) {
		btfs_rpc_free(sbi, req);
		return ret;
	}
	
	ret = btfs_rpc_wait_reply(req, NULL, 0);
	btfs_rpc_free(sbi, req);
	
	return ret;
}

static int btfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct btfs_sb_info *sbi = BTFS_SB(inode->i_sb);
	struct btfs_rpc_req *req;
	struct btfs_truncate_req truncate_req;
	u32 seq;
	int ret;
	
	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;
	
	if (attr->ia_valid & ATTR_SIZE) {
		btfs_get_path(dentry, truncate_req.path, BTFS_MAX_PATH);
		truncate_req.size = attr->ia_size;
		
		req = btfs_rpc_alloc(sbi);
		if (!req)
			return -ENOMEM;
		
		ret = btfs_rpc_send(sbi, BTFS_OP_TRUNCATE, &truncate_req,
				    sizeof(truncate_req), &seq);
		if (ret < 0) {
			btfs_rpc_free(sbi, req);
			return ret;
		}
		
		ret = btfs_rpc_wait_reply(req, NULL, 0);
		btfs_rpc_free(sbi, req);
		
		if (ret < 0)
			return ret;
		
		truncate_setsize(inode, attr->ia_size);
	}
	
	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	
	return 0;
}

static const struct inode_operations btfs_file_inode_operations = {
	.getattr	= btfs_getattr,
	.setattr	= btfs_setattr,
};

static const struct inode_operations btfs_dir_inode_operations = {
	.lookup		= btfs_lookup,
	.getattr	= btfs_getattr,
	.create		= btfs_create,
	.unlink		= btfs_unlink,
	.mkdir		= btfs_mkdir,
	.rmdir		= btfs_rmdir,
	.rename		= btfs_rename,
};

// ============================================================================
// Superblock Operations (like NFS super_operations)
// ============================================================================

static struct inode *btfs_alloc_inode(struct super_block *sb)
{
	struct btfs_inode_info *info;
	
	info = kmem_cache_alloc(btfs_inode_cachep, GFP_KERNEL);
	if (!info)
		return NULL;
	
	return &info->vfs_inode;
}

static void btfs_free_inode(struct inode *inode)
{
	kmem_cache_free(btfs_inode_cachep, BTFS_I(inode));
}

static void btfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void btfs_put_super(struct super_block *sb)
{
	struct btfs_sb_info *sbi = BTFS_SB(sb);
	struct btfs_rpc_req *req, *tmp;
	
	if (!sbi)
		return;
	
	// Remove from global list
	spin_lock(&btfs_sb_lock);
	if (btfs_current_sbi == sbi)
		btfs_current_sbi = NULL;
	spin_unlock(&btfs_sb_lock);
	
	// Wake up all pending requests
	spin_lock(&sbi->req_lock);
	list_for_each_entry_safe(req, tmp, &sbi->pending_reqs, list) {
		req->completed = true;
		req->result = -ESHUTDOWN;
		wake_up(&req->wait);
	}
	spin_unlock(&sbi->req_lock);
	
	synchronize_rcu();
	
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static const struct super_operations btfs_super_ops = {
	.alloc_inode	= btfs_alloc_inode,
	.free_inode	= btfs_free_inode,
	.evict_inode	= btfs_evict_inode,
	.put_super	= btfs_put_super,
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
};

// ============================================================================
// Mount/Unmount (like NFS mount)
// ============================================================================

static int btfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct btfs_sb_info *sbi;
	struct inode *root;
	
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	
	spin_lock_init(&sbi->req_lock);
	INIT_LIST_HEAD(&sbi->pending_reqs);
	atomic_set(&sbi->seq_counter, 0);
	sbi->daemon_alive = false;
	sbi->daemon_pid = 0;
	
	// Register as current superblock
	spin_lock(&btfs_sb_lock);
	btfs_current_sbi = sbi;
	spin_unlock(&btfs_sb_lock);
	
	sb->s_fs_info = sbi;
	sb->s_magic = BTFS_MAGIC;
	sb->s_op = &btfs_super_ops;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_time_gran = 1;
	sb->s_flags |= SB_NODIRATIME;
	
	root = new_inode(sb);
	if (!root) {
		spin_lock(&btfs_sb_lock);
		btfs_current_sbi = NULL;
		spin_unlock(&btfs_sb_lock);
		kfree(sbi);
		return -ENOMEM;
	}
	
	root->i_ino = 1;
	root->i_mode = S_IFDIR | 0755;
	root->i_op = &btfs_dir_inode_operations;
	root->i_fop = &btfs_dir_operations;
	set_nlink(root, 2);
	inode_init_owner(&nop_mnt_idmap, root, NULL, S_IFDIR | 0755);
	
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		spin_lock(&btfs_sb_lock);
		btfs_current_sbi = NULL;
		spin_unlock(&btfs_sb_lock);
		kfree(sbi);
		return -ENOMEM;
	}
	
	return 0;
}

static struct dentry *btfs_mount(struct file_system_type *fs_type, int flags,
				  const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, btfs_fill_super);
}

static void btfs_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);
}

static struct file_system_type btfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "btfs",
	.mount		= btfs_mount,
	.kill_sb	= btfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

// ============================================================================
// Module Init/Exit
// ============================================================================

static int __init btfs_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = btfs_nl_recv,
	};
	int ret;
	
	pr_info("btfs: Initializing BTFS client module\n");
	
	btfs_inode_cachep = kmem_cache_create("btfs_inode_cache",
					      sizeof(struct btfs_inode_info),
					      0,
					      SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					      btfs_inode_init_once);
	if (!btfs_inode_cachep) {
		pr_err("btfs: Failed to create inode cache\n");
		return -ENOMEM;
	}
	
	btfs_nl_sock = netlink_kernel_create(&init_net, NETLINK_BTFS, &cfg);
	if (!btfs_nl_sock) {
		pr_err("btfs: Failed to create netlink socket\n");
		kmem_cache_destroy(btfs_inode_cachep);
		return -ENOMEM;
	}
	
	ret = register_filesystem(&btfs_fs_type);
	if (ret) {
		pr_err("btfs: Failed to register filesystem\n");
		netlink_kernel_release(btfs_nl_sock);
		kmem_cache_destroy(btfs_inode_cachep);
		return ret;
	}
	
	pr_info("btfs: Module loaded successfully\n");
	return 0;
}

static void __exit btfs_exit(void)
{
	pr_info("btfs: Unloading BTFS client module\n");
	
	unregister_filesystem(&btfs_fs_type);
	
	if (btfs_nl_sock) {
		netlink_kernel_release(btfs_nl_sock);
		btfs_nl_sock = NULL;
	}
	
	rcu_barrier();
	kmem_cache_destroy(btfs_inode_cachep);
	
	pr_info("btfs: Module unloaded\n");
}

module_init(btfs_init);
module_exit(btfs_exit);

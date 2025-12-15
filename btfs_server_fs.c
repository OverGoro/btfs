// btfs_server_fs.c - Kernel module для сервера

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
#include <linux/namei.h>
#include <linux/mount.h>

#define BTFS_MAGIC 0x42544653
#define NETLINK_BTFS_SERVER 30
#define MAX_PENDING 100

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BTFS Team");
MODULE_DESCRIPTION("Bluetooth Distributed File System - Server");

// ============ FORWARD DECLARATIONS ============

// Inode operations
static int btfs_create(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
static struct dentry *btfs_lookup(struct inode *, struct dentry *, unsigned int);
static int btfs_unlink(struct inode *, struct dentry *);
static int btfs_mkdir(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
static int btfs_rmdir(struct inode *, struct dentry *);
static int btfs_getattr(struct mnt_idmap *, const struct path *, struct kstat *, u32, unsigned int);
static int btfs_setattr(struct mnt_idmap *, struct dentry *, struct iattr *);

// File operations
static int btfs_open(struct inode *, struct file *);
static int btfs_release(struct inode *, struct file *);
static ssize_t btfs_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t btfs_write(struct file *, const char __user *, size_t, loff_t *);
static int btfs_fsync(struct file *, loff_t, loff_t, int);

// Super operations
static struct inode *btfs_get_inode(struct super_block *, struct path *, umode_t);
static int btfs_fill_super(struct super_block *, void *, int);
static void btfs_kill_sb(struct super_block *);
static struct dentry *btfs_mount(struct file_system_type *, int, const char *, void *);

// Helper functions
static int check_lock_permission(struct inode *, int);

// Structures declarations
struct btfs_mount_opts;
struct btfs_fs_info;
struct btfs_inode_info;

// ============ STRUCTURES ============

// Структура монтирования
struct btfs_mount_opts {
    char *backing_dir;  // Реальная папка с данными
};

struct btfs_fs_info {
    struct btfs_mount_opts mount_opts;
    struct path backing_path;  // Path к реальной директории
};

// Структура для хранения информации об inode
struct btfs_inode_info {
    struct inode vfs_inode;
    struct path backing_path;  // Path к реальному файлу
};

// Структура для lock request
typedef struct {
    u32 operation;  // 1=check_lock, 2=acquire_lock, 3=release_lock
    u32 client_id;
    u32 lock_type;  // 1=read, 2=write
    char path[256];
} btfs_lock_request_t;

typedef struct {
    u32 allowed;  // 0=denied, 1=allowed
    u32 lock_id;
} btfs_lock_response_t;

// Netlink для общения с daemon
static struct sock *nl_sock = NULL;
static u32 daemon_pid = 0;
static u32 sequence_counter = 1;
static DEFINE_MUTEX(nl_mutex);

// Pending requests
typedef struct btfs_pending {
    u32 sequence;
    wait_queue_head_t wait;
    int completed;
    s32 result;
    void *data;
    u32 data_len;
    struct btfs_pending *next;
} btfs_pending_t;

static DEFINE_MUTEX(pending_mutex);
static btfs_pending_t *pending_head = NULL;

// Netlink message
typedef struct {
    u32 opcode;
    u32 sequence;
    s32 result;
    u32 data_len;
    char data[0];
} nl_message_t;

// ============ OPERATIONS TABLES (EARLY DECLARATION) ============

static const struct inode_operations btfs_dir_inode_ops = {
    .lookup = btfs_lookup,
    .create = btfs_create,
    .unlink = btfs_unlink,
    .mkdir = btfs_mkdir,
    .rmdir = btfs_rmdir,
};

static const struct inode_operations btfs_file_inode_ops = {
    .getattr = btfs_getattr,
    .setattr = btfs_setattr,
};

static const struct file_operations btfs_file_ops = {
    .open = btfs_open,
    .release = btfs_release,
    .read = btfs_read,
    .write = btfs_write,
    .fsync = btfs_fsync,
    .llseek = generic_file_llseek,
};

static const struct super_operations btfs_super_ops = {
    .statfs = simple_statfs,
    .drop_inode = generic_delete_inode,
};

static struct file_system_type btfs_server_fs_type = {
    .owner = THIS_MODULE,
    .name = "btfs_server",
    .mount = btfs_mount,
    .kill_sb = btfs_kill_sb,
};

// ============ PENDING MANAGEMENT ============

static btfs_pending_t *create_pending(u32 sequence) {
    btfs_pending_t *pending = kmalloc(sizeof(btfs_pending_t), GFP_KERNEL);
    if (!pending) return NULL;
    
    pending->sequence = sequence;
    pending->completed = 0;
    pending->result = 0;
    pending->data = NULL;
    pending->data_len = 0;
    init_waitqueue_head(&pending->wait);
    
    mutex_lock(&pending_mutex);
    pending->next = pending_head;
    pending_head = pending;
    mutex_unlock(&pending_mutex);
    
    return pending;
}

static btfs_pending_t *find_pending(u32 sequence) {
    btfs_pending_t *pending;
    
    mutex_lock(&pending_mutex);
    pending = pending_head;
    while (pending) {
        if (pending->sequence == sequence) {
            mutex_unlock(&pending_mutex);
            return pending;
        }
        pending = pending->next;
    }
    mutex_unlock(&pending_mutex);
    
    return NULL;
}

static void remove_pending(u32 sequence) {
    btfs_pending_t *pending, *prev = NULL;
    
    mutex_lock(&pending_mutex);
    pending = pending_head;
    
    while (pending) {
        if (pending->sequence == sequence) {
            if (prev) {
                prev->next = pending->next;
            } else {
                pending_head = pending->next;
            }
            
            if (pending->data) {
                kfree(pending->data);
            }
            kfree(pending);
            break;
        }
        prev = pending;
        pending = pending->next;
    }
    
    mutex_unlock(&pending_mutex);
}

// ============ NETLINK ============

static int send_to_daemon(u32 opcode, const void *data, u32 data_len, 
                          u32 *sequence_out) {
    struct sk_buff *skb;
    struct nlmsghdr *nlh;
    nl_message_t *msg;
    int ret;
    
    if (!daemon_pid) {
        return -ENOTCONN;
    }
    
    mutex_lock(&nl_mutex);
    
    *sequence_out = sequence_counter++;
    
    size_t total_len = NLMSG_SPACE(sizeof(nl_message_t) + data_len);
    skb = nlmsg_new(total_len, GFP_KERNEL);
    if (!skb) {
        mutex_unlock(&nl_mutex);
        return -ENOMEM;
    }
    
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, total_len - NLMSG_HDRLEN, 0);
    if (!nlh) {
        kfree_skb(skb);
        mutex_unlock(&nl_mutex);
        return -ENOMEM;
    }
    
    msg = (nl_message_t *)nlmsg_data(nlh);
    msg->opcode = opcode;
    msg->sequence = *sequence_out;
    msg->result = 0;
    msg->data_len = data_len;
    
    if (data && data_len > 0) {
        memcpy(msg->data, data, data_len);
    }
    
    ret = nlmsg_unicast(nl_sock, skb, daemon_pid);
    mutex_unlock(&nl_mutex);
    
    return (ret < 0) ? ret : 0;
}

static void netlink_recv(struct sk_buff *skb) {
    struct nlmsghdr *nlh;
    nl_message_t *msg;
    btfs_pending_t *pending;
    
    nlh = (struct nlmsghdr *)skb->data;
    msg = (nl_message_t *)nlmsg_data(nlh);
    
    if (!daemon_pid) {
        daemon_pid = nlh->nlmsg_pid;
        printk(KERN_INFO "btfs_server: Daemon connected (PID=%u)\n", daemon_pid);
    }
    
    pending = find_pending(msg->sequence);
    if (!pending) {
        return;
    }
    
    pending->result = msg->result;
    
    if (msg->data_len > 0) {
        pending->data = kmalloc(msg->data_len, GFP_KERNEL);
        if (pending->data) {
            memcpy(pending->data, msg->data, msg->data_len);
            pending->data_len = msg->data_len;
        }
    }
    
    pending->completed = 1;
    wake_up_interruptible(&pending->wait);
}

static int wait_for_response(u32 sequence, s32 *result, void *data_buffer, 
                             u32 buffer_size, u32 *data_len_out) {
    btfs_pending_t *pending;
    int ret;
    
    pending = find_pending(sequence);
    if (!pending) {
        return -EINVAL;
    }
    
    ret = wait_event_interruptible_timeout(pending->wait, 
                                           pending->completed,
                                           10 * HZ);
    
    if (ret == 0) {
        remove_pending(sequence);
        return -ETIMEDOUT;
    }
    
    if (ret < 0) {
        remove_pending(sequence);
        return ret;
    }
    
    *result = pending->result;
    
    if (data_buffer && buffer_size > 0 && pending->data && pending->data_len > 0) {
        u32 copy_len = min(pending->data_len, buffer_size);
        memcpy(data_buffer, pending->data, copy_len);
        if (data_len_out) {
            *data_len_out = copy_len;
        }
    }
    
    remove_pending(sequence);
    return 0;
}

// ============ LOCK CHECKING ============

static int check_lock_permission(struct inode *inode, int access_mode) {
    struct dentry *dentry = d_find_alias(inode);
    char *path_buf, *pathname;
    btfs_lock_request_t lock_req;
    btfs_lock_response_t lock_resp;
    u32 sequence;
    s32 result;
    u32 resp_len;
    int ret;
    btfs_pending_t *pending;
    
    if (!dentry) {
        return 0;
    }
    
    path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!path_buf) {
        dput(dentry);
        return -ENOMEM;
    }
    
    pathname = dentry_path_raw(dentry, path_buf, PATH_MAX);
    if (IS_ERR(pathname)) {
        kfree(path_buf);
        dput(dentry);
        return 0;
    }
    
    memset(&lock_req, 0, sizeof(lock_req));
    lock_req.operation = 1;
    lock_req.client_id = current->pid;
    lock_req.lock_type = (access_mode & MAY_WRITE) ? 2 : 1;
    strncpy(lock_req.path, pathname, sizeof(lock_req.path) - 1);
    
    pending = create_pending(sequence);
    if (!pending) {
        kfree(path_buf);
        dput(dentry);
        return -ENOMEM;
    }
    
    ret = send_to_daemon(100, &lock_req, sizeof(lock_req), &sequence);
    if (ret < 0) {
        remove_pending(sequence);
        kfree(path_buf);
        dput(dentry);
        return ret;
    }
    
    ret = wait_for_response(sequence, &result, &lock_resp, 
                           sizeof(lock_resp), &resp_len);
    
    kfree(path_buf);
    dput(dentry);
    
    if (ret < 0) {
        return ret;
    }
    
    if (result < 0) {
        return result;
    }
    
    if (!lock_resp.allowed) {
        printk(KERN_DEBUG "btfs_server: Access denied due to lock\n");
        return -EACCES;
    }
    
    return 0;
}

// ============ FILE OPERATIONS ============

static int btfs_open(struct inode *inode, struct file *file) {
    struct btfs_inode_info *info = container_of(inode, struct btfs_inode_info, vfs_inode);
    struct file *backing_file;
    int flags = file->f_flags;
    int ret;
    
    printk(KERN_DEBUG "btfs_server: open inode=%lu, flags=%x\n", 
           inode->i_ino, flags);
    
    int access_mode = (flags & O_WRONLY || flags & O_RDWR) ? MAY_WRITE : MAY_READ;
    ret = check_lock_permission(inode, access_mode);
    if (ret < 0) {
        return ret;
    }
    
    backing_file = dentry_open(&info->backing_path, flags, current_cred());
    if (IS_ERR(backing_file)) {
        return PTR_ERR(backing_file);
    }
    
    file->private_data = backing_file;
    
    return 0;
}

static int btfs_release(struct inode *inode, struct file *file) {
    struct file *backing_file = file->private_data;
    
    if (backing_file) {
        fput(backing_file);
    }
    
    return 0;
}

static ssize_t btfs_read(struct file *file, char __user *buf, 
                         size_t count, loff_t *ppos) {
    struct file *backing_file = file->private_data;
    
    if (!backing_file) {
        return -EINVAL;
    }
    
    return kernel_read(backing_file, buf, count, ppos);
}

static ssize_t btfs_write(struct file *file, const char __user *buf,
                          size_t count, loff_t *ppos) {
    struct file *backing_file = file->private_data;
    
    if (!backing_file) {
        return -EINVAL;
    }
    
    int ret = check_lock_permission(file->f_inode, MAY_WRITE);
    if (ret < 0) {
        return ret;
    }
    
    return kernel_write(backing_file, buf, count, ppos);
}

static int btfs_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
    struct file *backing_file = file->private_data;
    
    if (!backing_file || !backing_file->f_op->fsync) {
        return -EINVAL;
    }
    
    return backing_file->f_op->fsync(backing_file, start, end, datasync);
}

// ============ INODE OPERATIONS ============

static int btfs_create(struct mnt_idmap *idmap, struct inode *dir, 
                       struct dentry *dentry, umode_t mode, bool excl) {
    struct btfs_inode_info *dir_info = container_of(dir, struct btfs_inode_info, vfs_inode);
    struct path backing_path;
    struct dentry *backing_dentry;
    struct inode *inode;
    int ret;
    
    printk(KERN_DEBUG "btfs_server: create %s\n", dentry->d_name.name);
    
    backing_dentry = lookup_one_len(dentry->d_name.name, 
                                    dir_info->backing_path.dentry,
                                    dentry->d_name.len);
    
    if (IS_ERR(backing_dentry)) {
        return PTR_ERR(backing_dentry);
    }
    
    ret = vfs_create(&nop_mnt_idmap, d_inode(dir_info->backing_path.dentry), 
                     backing_dentry, mode, excl);
    
    if (ret < 0) {
        dput(backing_dentry);
        return ret;
    }
    
    backing_path.dentry = backing_dentry;
    backing_path.mnt = dir_info->backing_path.mnt;
    
    inode = btfs_get_inode(dir->i_sb, &backing_path, S_IFREG | mode);
    if (!inode) {
        dput(backing_dentry);
        return -ENOMEM;
    }
    
    d_instantiate(dentry, inode);
    
    return 0;
}

static struct dentry *btfs_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags) {
    struct btfs_inode_info *dir_info = container_of(dir, struct btfs_inode_info, vfs_inode);
    struct dentry *backing_dentry;
    struct path backing_path;
    struct inode *inode = NULL;
    
    printk(KERN_DEBUG "btfs_server: lookup %s\n", dentry->d_name.name);
    
    backing_dentry = lookup_one_len(dentry->d_name.name,
                                    dir_info->backing_path.dentry,
                                    dentry->d_name.len);
    
    if (IS_ERR(backing_dentry)) {
        return backing_dentry;
    }
    
    if (d_really_is_positive(backing_dentry)) {
        backing_path.dentry = backing_dentry;
        backing_path.mnt = dir_info->backing_path.mnt;
        
        inode = btfs_get_inode(dir->i_sb, &backing_path, 
                              d_inode(backing_dentry)->i_mode);
    }
    
    dput(backing_dentry);
    
    return d_splice_alias(inode, dentry);
}

static int btfs_unlink(struct inode *dir, 
                       struct dentry *dentry) {
    struct btfs_inode_info *dir_info = container_of(dir, struct btfs_inode_info, vfs_inode);
    struct dentry *backing_dentry;
    int ret;
    
    printk(KERN_DEBUG "btfs_server: unlink %s\n", dentry->d_name.name);
    
    backing_dentry = lookup_one_len(dentry->d_name.name,
                                    dir_info->backing_path.dentry,
                                    dentry->d_name.len);
    
    if (IS_ERR(backing_dentry)) {
        return PTR_ERR(backing_dentry);
    }
    
    ret = vfs_unlink(&nop_mnt_idmap, d_inode(dir_info->backing_path.dentry), 
                     backing_dentry, NULL);
    
    dput(backing_dentry);
    
    return ret;
}

static int btfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                      struct dentry *dentry, umode_t mode) {
    struct btfs_inode_info *dir_info = container_of(dir, struct btfs_inode_info, vfs_inode);
    struct dentry *backing_dentry;
    int ret;
    
    backing_dentry = lookup_one_len(dentry->d_name.name,
                                    dir_info->backing_path.dentry,
                                    dentry->d_name.len);
    
    if (IS_ERR(backing_dentry)) {
        return PTR_ERR(backing_dentry);
    }
    
    ret = vfs_mkdir(&nop_mnt_idmap, d_inode(dir_info->backing_path.dentry),
                    backing_dentry, mode);
    
    dput(backing_dentry);
    return ret;
}

static int btfs_rmdir(struct inode *dir,
                      struct dentry *dentry) {
    struct btfs_inode_info *dir_info = container_of(dir, struct btfs_inode_info, vfs_inode);
    struct dentry *backing_dentry;
    int ret;
    
    backing_dentry = lookup_one_len(dentry->d_name.name,
                                    dir_info->backing_path.dentry,
                                    dentry->d_name.len);
    
    if (IS_ERR(backing_dentry)) {
        return PTR_ERR(backing_dentry);
    }
    
    ret = vfs_rmdir(&nop_mnt_idmap, d_inode(dir_info->backing_path.dentry),
                    backing_dentry);
    
    dput(backing_dentry);
    return ret;
}

static int btfs_getattr(struct mnt_idmap *idmap, const struct path *path,
                        struct kstat *stat, u32 request_mask, unsigned int flags) {
    struct inode *inode = d_inode(path->dentry);
    
    generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
    return 0;
}

static int btfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                        struct iattr *attr) {
    struct inode *inode = d_inode(dentry);
    int ret;
    
    ret = setattr_prepare(&nop_mnt_idmap, dentry, attr);
    if (ret)
        return ret;
    
    setattr_copy(&nop_mnt_idmap, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

// ============ SUPER OPERATIONS ============

static struct inode *btfs_get_inode(struct super_block *sb, 
                                    struct path *backing_path,
                                    umode_t mode) {
    struct btfs_inode_info *info;
    struct inode *inode;
    struct inode *backing_inode = d_inode(backing_path->dentry);
    
    inode = new_inode(sb);
    if (!inode) {
        return NULL;
    }
    
    info = container_of(inode, struct btfs_inode_info, vfs_inode);
    info->backing_path = *backing_path;
    path_get(&info->backing_path);
    
    inode->i_ino = get_next_ino();
    inode->i_mode = backing_inode->i_mode;
    inode->i_uid = backing_inode->i_uid;
    inode->i_gid = backing_inode->i_gid;
    inode->i_size = backing_inode->i_size;
    
    inode_set_atime_to_ts(inode, inode_get_atime(backing_inode));
    inode_set_mtime_to_ts(inode, inode_get_mtime(backing_inode));
    inode_set_ctime_to_ts(inode, inode_get_ctime(backing_inode));
    
    if (S_ISDIR(mode)) {
        inode->i_op = &btfs_dir_inode_ops;
        inode->i_fop = &simple_dir_operations;
        inc_nlink(inode);
    } else {
        inode->i_op = &btfs_file_inode_ops;
        inode->i_fop = &btfs_file_ops;
    }
    
    return inode;
}

static int btfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct btfs_fs_info *fsi;
    struct inode *root_inode;
    struct dentry *root_dentry;
    int ret;
    
    fsi = kzalloc(sizeof(struct btfs_fs_info), GFP_KERNEL);
    if (!fsi) {
        return -ENOMEM;
    }
    
    sb->s_fs_info = fsi;
    
    fsi->mount_opts.backing_dir = kstrdup("/tmp/btfs_backing", GFP_KERNEL);
    
    ret = kern_path(fsi->mount_opts.backing_dir, LOOKUP_FOLLOW, 
                    &fsi->backing_path);
    if (ret) {
        printk(KERN_ERR "btfs_server: Invalid backing directory\n");
        kfree(fsi->mount_opts.backing_dir);
        kfree(fsi);
        return ret;
    }
    
    sb->s_magic = BTFS_MAGIC;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_op = &btfs_super_ops;
    
    root_inode = btfs_get_inode(sb, &fsi->backing_path, S_IFDIR | 0755);
    if (!root_inode) {
        path_put(&fsi->backing_path);
        kfree(fsi->mount_opts.backing_dir);
        kfree(fsi);
        return -ENOMEM;
    }
    
    root_dentry = d_make_root(root_inode);
    if (!root_dentry) {
        path_put(&fsi->backing_path);
        kfree(fsi->mount_opts.backing_dir);
        kfree(fsi);
        return -ENOMEM;
    }
    
    sb->s_root = root_dentry;
    
    printk(KERN_INFO "btfs_server: Mounted on backing dir: %s\n",
           fsi->mount_opts.backing_dir);
    
    return 0;
}

static void btfs_kill_sb(struct super_block *sb) {
    struct btfs_fs_info *fsi = sb->s_fs_info;
    
    if (fsi) {
        path_put(&fsi->backing_path);
        kfree(fsi->mount_opts.backing_dir);
        kfree(fsi);
    }
    
    kill_litter_super(sb);
}

static struct dentry *btfs_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev_name, void *data) {
    return mount_nodev(fs_type, flags, data, btfs_fill_super);
}

// ============ MODULE INIT/EXIT ============

static int __init btfs_server_init(void) {
    int ret;
    
    struct netlink_kernel_cfg cfg = {
        .input = netlink_recv,
    };
    
    printk(KERN_INFO "btfs_server: Initializing\n");
    
    nl_sock = netlink_kernel_create(&init_net, NETLINK_BTFS_SERVER, &cfg);
    if (!nl_sock) {
        return -ENOMEM;
    }
    
    ret = register_filesystem(&btfs_server_fs_type);
    if (ret) {
        netlink_kernel_release(nl_sock);
        return ret;
    }
    
    printk(KERN_INFO "btfs_server: Module loaded\n");
    printk(KERN_INFO "btfs_server: Mount: mount -t btfs_server -o backing_dir=/real/path none /mnt/btfs\n");
    
    return 0;
}

static void __exit btfs_server_exit(void) {
    unregister_filesystem(&btfs_server_fs_type);
    
    if (nl_sock) {
        netlink_kernel_release(nl_sock);
    }
    
    printk(KERN_INFO "btfs_server: Unloaded\n");
}

module_init(btfs_server_init);
module_exit(btfs_server_exit);

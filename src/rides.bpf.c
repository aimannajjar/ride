#include "rides.h"
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
// #include <fcntl.h>

// struct path {
// 	struct vfsmount *mnt;
// 	struct dentry *dentry;
// }__attribute__((preserve_access_index));

// struct file {
// 	spinlock_t f_lock;
// fmode_t f_mode;
// 	const struct file_operations *f_op;
// 	struct address_space *f_mapping;
// 	void *private_data;
// 	struct inode *f_inode;
// 	unsigned int f_flags;
// 	unsigned int f_iocb_flags;
// 	const struct cred *f_cred;
// 	struct fown_struct *f_owner;
// union {
// 	const struct path f_path;
// 	struct path __f_path;
// };
// 	union {
// 		struct mutex f_pos_lock;
// 		u64 f_pipe;
// 	};
// 	loff_t f_pos;
// 	void *f_security;
// 	errseq_t f_wb_err;
// 	errseq_t f_sb_err;
// 	struct hlist_head *f_ep;
// 	union {
// 		struct callback_head f_task_work;
// 		struct llist_node f_llist;
// 		struct file_ra_state f_ra;
// 		freeptr_t f_freeptr;
// 	};
// 	file_ref_t f_ref;
// }__attribute__((preserve_access_index));
//
//
// union shortname_store {
// 	unsigned char string[40];
// 	long unsigned int words[5];
// };
//
//
// struct dentry {
// 	// unsigned int d_flags;
// 	// seqcount_spinlock_t d_seq;
// 	// struct hlist_bl_node d_hash;
// 	struct dentry *d_parent;
// 	union shortname_store d_shortname;
// 	// union {
// 	// 	struct qstr __d_name;
// 	// 	const struct qstr d_name;
// 	// };
// 	struct inode *d_inode;
// }__attribute__((preserve_access_index));

#define S_IFMT 0170000 /* These bits determine file type.  */

/* File types */
#define S_IFDIR 0040000  /* Directory.  */
#define S_IFCHR 0020000  /* Character device.  */
#define S_IFBLK 0060000  /* Block device.  */
#define S_IFREG 0100000  /* Regular file.  */
#define S_IFIFO 0010000  /* FIFO.  */
#define S_IFLNK 0120000  /* Symbolic link.  */
#define S_IFSOCK 0140000 /* Socket.  */

/* POSIX.1b objects.  Note that these macros always evaluate to zero.  But
   they do it by enforcing the correct use of the macros.  */
#define S_TYPEISMQ(buf) ((buf)->st_mode - (buf)->st_mode)
#define S_TYPEISSEM(buf) ((buf)->st_mode - (buf)->st_mode)
#define S_TYPEISSHM(buf) ((buf)->st_mode - (buf)->st_mode)

/* Protection bits.  */
#define S_ISUID 04000 /* Set user ID on execution.  */
#define S_ISGID 02000 /* Set group ID on execution.  */
#define S_ISVTX 01000 /* Save swapped text after use (sticky).  */
#define S_IREAD 0400  /* Read by owner.  */
#define S_IWRITE 0200 /* Write by owner.  */
#define S_IEXEC 0100  /* Execute by owner.  */

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define FS_REQUIRES_DEV 1

struct {
  __uint(type, BPF_MAP_TYPE_QUEUE);
  __uint(max_entries, 16);
  __type(value, struct event);
} queue SEC(".maps");

bool fs_requires_dev(struct super_block *sb) {
  return sb->s_type && (sb->s_type->fs_flags & FS_REQUIRES_DEV);
}

SEC("lsm/file_open")
int BPF_PROG(rides, struct file *file) {

  if (!S_ISREG(file->f_inode->i_mode) ||
      !fs_requires_dev(file->f_inode->i_sb)) {
    return 0;
  }

  struct event ev;
  bpf_path_d_path(&file->f_path, ev.path, sizeof(ev.path));
  bpf_map_push_elem(&queue, &ev, 0);

  return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";

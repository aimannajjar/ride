#include "ride.h"
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define S_IFMT 0170000 /* These bits determine file type.  */
#define S_IFREG 0100000  /* Regular file.  */
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define FS_REQUIRES_DEV 1

struct {
  __uint(type, BPF_MAP_TYPE_QUEUE);
  __uint(max_entries, BPF_RING_BUF_SIZE);
  __type(value, struct event);
} queue SEC(".maps");

bool fs_requires_dev(struct super_block *sb) {
  return sb->s_type && (sb->s_type->fs_flags & FS_REQUIRES_DEV);
}

SEC("lsm/file_open")
int BPF_PROG(ride, struct file *file) {

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

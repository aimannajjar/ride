// SPDX-License-Identifier: (MIT OR GPL-2.0)
#include "ride.h"
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define S_IFMT 0170000  /* These bits determine file type.  */
#define S_IFREG 0100000 /* Regular file.  */
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define FS_REQUIRES_DEV 1


#define RINGBUF_SIZE (MAX_FILENAME_LEN * 1024)
const volatile char watch_path[MAX_FILENAME_LEN];
const volatile size_t watch_path_len;
const volatile enum watch_path_type watch_path_type;
const volatile pid_t userspace_pid;

struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, RINGBUF_SIZE);
} rb SEC(".maps");

bool fs_requires_dev(struct super_block *sb) {
  return sb->s_type && (sb->s_type->fs_flags & FS_REQUIRES_DEV);
}

bool in_watch_path(const char *path, int plen) {
#ifdef BPF_DEBUG
  char fmt[] = "Comparing %s with %s\n";
  bpf_trace_printk(fmt, sizeof(fmt), path, watch_path);
#endif

  int i = 0;

  if (plen < watch_path_len ||
      (watch_path_type == WATCH_FILE && watch_path_len != plen)) {

#ifdef BPF_DEBUG
    char fmt[] = "Length mismatch (event<>watch_path): %d <> %d\n";
    bpf_trace_printk(fmt, sizeof(fmt), plen, watch_path_len);
#endif
    return false;
  }

  for (i = 0; i < watch_path_len; i++) {
    if (path[i] != watch_path[i])
      return false;
  }
  return true;
}

SEC("lsm/file_open")
int BPF_PROG(ride, struct file *file) {
  if (bpf_get_current_pid_tgid() >> 32 == userspace_pid)
    return 0;

  if (!S_ISREG(file->f_inode->i_mode) ||
      !fs_requires_dev(file->f_inode->i_sb)) {
    return 0;
  }

  struct event *ev;
  ev = bpf_ringbuf_reserve(&rb, sizeof(*ev), 0);
  if (!ev) {
    // TODO: account drops
    return 0;
  }
  int plen = bpf_path_d_path(&file->f_path, ev->path, sizeof(ev->path));
  plen -= 1; // bpf_path_d_path returns size, i.e. nul included, we want length
  ev->path[sizeof(ev->path) - 1] = '\0';

  if (!in_watch_path(ev->path, plen)) {
    bpf_ringbuf_discard(ev, 0);
    return 0;
  }

  bpf_ringbuf_submit(ev, BPF_RB_FORCE_WAKEUP);

  return 0;
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";

# RIDE (Real-time Intrusion Detection Events)

An eBPF-based modern approach to File Integrity Monitoring (FIM):

- This project aims to implement next-generation, AIDE-style intrusion detection.
- Uses the latest algorithms for efficient, high-speed hashing (BLAKE3).
- Real-time events triggered by an eBPF LSM kernel program, paired with an
  efficient, high-throughput userspace component to process the events:
  - eBPF communicates with userspace via a shared ring buffer (mmap'd) =
    zero-copy and no syscalls!
  - eBPF hooks tirggers in kernel = more reliable and faster than than inotify.
  - Multi-threded userspace to process event for higher throughput.
- Plans to support various "stores" for the verification database, including:
  - File-based (similar to AIDE)
  - Remote over UDP or other transports
- Written in C for a lightweight, efficient binary; remote stores may be
  implemented in Rust for broader library availability.


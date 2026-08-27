# RIDE (Real-time Intrusion Detection Events)

An eBPF-based modern approach to File Integrity Monitoring (FIM):

- This project aims to implement next-generation, AIDE-style intrusion
  detection.
- Uses the latest algorithms for efficient, high-speed hashing (BLAKE3).
- eBPF in-kernel program communicates with userspace via a shared ring 
  buffer (mmap'd), effectively achieving zero-copy and no syscalls per event
- On a ThinkPad E40 with an SSD on btrfs, it currently measures about
  500 files/sec when reading 1000 non-cached 8KB files:
  - High throughput makes it practical to continuously monitor a broad range of
    files, including system binaries, config files, and data files.
- Plans to support various "stores" for the verification database,
  including:
  - File-based (similar to AIDE)
  - Remote over UDP or other transports
  - Hardware-based attestation

Note that RIDE is not an enterprise-style EDR: its simpler, UNIX-like,
single-purpose architecture makes it more accessible and embeddable (e.g., in
containers).

## Modern, eBPF-based, high-throughput monitoring

Classic FIM tools scan periodically: the larger the set of watched
files, the longer the scanning takes. Instead, RIDE verifies
file events as they happen; and its high-throughput implementation makes
it practical to watch basically all file events continuously:

- Coverage is no longer limited to a small set of files: binaries,
  configuration files, and data files can all be watched continuously.
- Tampering is detected at the moment of use, when a file is opened,
  executed, or otherwise accessed, as opposed to the next scheduled scan.
- Events are delivered through kernel hooks instead of periodic
  full-filesystem sweeps, so the steady-state cost stays low even with
  a large watch set.

## Implementation

- Real-time events triggered by an eBPF LSM kernel program, paired with
  an efficient, high-throughput userspace component to process the
  events:
  - eBPF communicates with userspace via a shared ring buffer (mmap'd) =
    zero-copy and no per-event syscalls!
  - eBPF hooks trigger in the kernel = more reliable and faster than
    inotify.
  - One can theoretically attach eBPF programs to additional hooks to
    verify file integrity at different points, such as on open, delete,
    or link.
- Multi-threaded, async-IO-capable userspace to process events for
  higher throughput:
  - Each instance is capable of up to 32 concurrent IO reads.
  - Multiplied by the number of threads.
- Written in C for a lightweight, efficient binary; remote stores may be
  implemented in Rust to support a richer set of features on the
  verifier side.

## Examples of attacks that could be detected

- Polluted page-cache content vs. the baseline checksum, detecting
  page-cache-tampering vulnerabilities of the DirtyFrag / CopyFail /
  DirtyPipe class, i.e. CVE-2026-43284, CVE-2026-43500, CVE-2026-31431,
  CVE-2022-0847 (DirtyPipe), CVE-2016-5195 (Dirty COW).
- Supply-chain incidents, which are often silent and undetected until
  too late:
  - XZ Utils backdoor (CVE-2024-3094): trojanized liblzma shipped in
    downstream release tarballs.
  - SolarWinds Orion (SUNBURST), npm event-stream (2018), Polyfill.io
    (2024): tampered build or distribution artifacts.
- Configuration-file tampering:
  - SSH backdoors: appended keys in `~/.ssh/authorized_keys`, or
    `sshd_config` changes (`PermitRootLogin`, `PasswordAuthentication`).
  - Account backdoors via direct edits to `/etc/passwd`, `/etc/shadow`,
    `/etc/group`, or `/etc/sudoers`.
  - Persistence via new or modified systemd units, cron entries, or
    shell startup files (`/etc/profile.d/`, `~/.bashrc`).
  - Userland rootkits hijacking the dynamic linker through
    `/etc/ld.so.preload` (the Jynx/Azazel class).
  - Rogue package repositories added under `sources.list.d/` or
    `yum.repos.d/`, enabling supply-chain compromise via updates.
- Data-file tampering, impractical to watch with periodic scanning:
  - Web shells dropped into webroots or modified CMS core/plugin files
    (a common WordPress post-compromise pattern).
  - Unexpected modification of rotated/archived log files to erase
    evidence of earlier activity.

## Other tools

- pagecache-guard: monitors a specific class of files (SUID/SGID) via
  fanotify; aims to detect and protect against binary tampering.
- Linux IMA/EVM: powerful in-kernel file measurement and appraisal,
  but host-based, with a very complex setup and a steep learning curve.
- Samhain / Tripwire / AIDE: classic host-based FIM with periodic
  scanning — no real-time detection.
- Falco / Tracee / Tetragon: eBPF-based runtime security platforms;
  general-purpose rule engines, far heavier to deploy and operate.
- Wazuh / OSSEC: full HIDS suites with an FIM module; agent-manager
  architecture aimed at fleets.

## AI Policy

Given the sensitive nature of this security tool, contributors must fully
understand every line they submit. This project therefore does not accept
AI-assisted **code** contributions. However, AI tools may be used for
supporting tasks such as code review, research, and documentation.

## LICENSE
Apache 2.0



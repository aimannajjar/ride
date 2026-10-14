# RIDE (Real-time Intrusion Detection Events)
RIDE is a work-in-progress security tool that aims to create next-gen FIM software. People familiar with Unix security will recognize the name AIDE, a traditional and proven tool to detect suspicious writes on files. AIDE works by creating a baseline database of file hashes, which establishes a last known good state. You can then run AIDE periodically to verify hashes have not changed.

RIDE aims to modernize this approach, as computing has evolved significantly since the idea of AIDE was born in 1999. With new hashing algorithms, such as BLAKE3, the advent of NVMe SSDs and new capabilities of processors (SIMD) and operating systems, I believe we can create FIM software for **everyone** that can monitor and detect file changes in **real-time**, with low-latency and high-throughput to detect suspicious writes early. By that, I mean open-source, easy-to-use software that is not limited to complex enterprise infrastructure. Modern software with the Unix philosophy at its core.

## Approach
RIDE consists of two parts:
- Hasher (client-side): Uses Linux eBPF hooks to hash files as soon as they are accessed and submit the hashes to the verifier
- Verifier (server-side): Receives and verifies hashes. It can be set to learning mode for the initial baseline.

### Implementation
- The Hasher is written in C, the low-level access allows me to greatly tune for throughput and low-latency. The hasher relies on a hand-rolled `io_uring` implementation (I do not use liburing, or higher-level libraries like libuv), this allowed me to minimize latency using features not exposed by higher-level libraries. For instance, the hasher creates a separate `io_uring` instance per thread, with one of them in SQPOLL mode and others in normal mode. The SQPOLL ring allows the primary worker to submit requests with almost no syscalls, whereas the other workers are only used under increased load (this optimization is still WIP)

- The Verifier is written in Go. I plan to implement a rich set of features at the verifier level, such as a web backend and interface for reports, various alerting plugins, all of which are a good git for Go's ecosystem. Go's concurrency model is also well-suited to lightweight tasks such as hash verification. Network communication will be implemented on top of QUIC protocol (leveraging `ngtcp2` for the client, `quic-go` for the server). 


### Current Results
Despite being in early stages, RIDE is currently performing well within a practical range for its goals. I currently get 2000-2200 files hashed per second on a ThinkPad E14. This means you might be able to monitor your entire `/usr/bin` and `/etc` for suspicious writes. I plan to implement a feature where each file is hashed twice, once using the file from page cache and once via `O_DIRECT`. This would help detect attacks that exploit page cache bugs such as `DirtyFrag`, `DirtyPipe` and `DirtyCOW`.


## AI Policy
Given the sensitive nature of RIDE, i.e. as a security tool, I do not accept AI-generated code or any text used for human communication such as documentation, pull requests or commit messages. AI, however, may be used to review one's own work or documentation, or for research.

# RIDE (Real-time Intrusion Detection Events)

An eBPF-based modern approach to File Integrity Monitoring (FIM):

- This project aims to implement next-generation, AIDE-style intrusion detection.
- Uses the latest algorithms for efficient, high-speed hashing (Blake3).
- Real-time events triggered by an eBPF LSM kernel program, paired with an efficient, high-throughput userspace component to process the events.
- Plans to support various "stores" for the verification database, including:
  - File-based (similar to AIDE)
  - Remote over UDP or other transports
- Written in C for a lightweight, efficient binary; remote stores may be
  implemented in Rust for broader library availability.


## License
TBD. The project is currently source-available but will be released under an
open-source license in the future.

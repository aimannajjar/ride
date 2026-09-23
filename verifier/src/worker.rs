use std::{
    cmp::Reverse,
    collections::{BinaryHeap, HashMap},
    ffi::{CStr, c_int},
    fmt::Display,
    mem::{self, MaybeUninit},
    net::SocketAddr,
    os::fd::AsRawFd,
    process,
    rc::Rc,
    time::Instant,
};

use log::{debug, error, info, trace, warn};
use quiche::RecvInfo;
use socket2::{Domain, Protocol, SockAddr, Socket, Type};

use crate::{
    ffi::*,
    store::{HASH_LEN, Store},
};

pub(crate) const MAX_FILENAME_LEN: usize = 128;

struct RideClient {
    conn: quiche::Connection,
    last_timeout: Option<Instant>,
}

pub(crate) struct Worker<'a> {
    buffer: [MaybeUninit<u8>; 65535],
    socket: Socket,
    clients: HashMap<Rc<[u8]>, RideClient>,
    config: quiche::Config,
    local_address: SocketAddr,
    timeouts: BinaryHeap<Reverse<(Instant, Rc<[u8]>)>>,
    epfd: c_int,
    sfd: i32,
    store: &'a Store,
}

struct Message {
    pub(crate) hash: [u8; HASH_LEN],
    pub(crate) path: String,
}

impl Display for Message {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "file={} hash={}", self.path, hex::encode(self.hash))
    }
}

#[derive(Debug)]
#[allow(dead_code)]
enum MessageError {
    HashParseError(&'static str),
    PathParseError(&'static str),
}

impl TryFrom<&mut [u8]> for Message {
    type Error = MessageError;

    fn try_from(value: &mut [u8]) -> Result<Self, Self::Error> {
        let hash = *value
            .first_chunk::<HASH_LEN>()
            .ok_or(MessageError::HashParseError("hash out of bounds"))?;

        let path = value[HASH_LEN..]
            .get(..MAX_FILENAME_LEN)
            .ok_or(MessageError::PathParseError("path out of bounds"))?;

        let path = CStr::from_bytes_until_nul(path)
            .map_err(|_| MessageError::PathParseError("parsing C string failed"))?
            .to_str()
            .map_err(|_| MessageError::PathParseError("path contained invalid UTF-8"))?
            .to_string();

        Ok(Message { hash, path })
    }
}

const ADDRESS: &str = "127.0.0.1:4433";
const RIDE_ALPN: &[&[u8]] = &[b"ride0.1"];
const MAXEVENTS: usize = 10;
const MAX_DATAGRAM_SIZE: usize = 1350;

impl<'a> Worker<'a> {
    pub(crate) fn new(store: &'a Store, sfd: i32) -> Self {
        let buffer: [MaybeUninit<u8>; 65535] = [MaybeUninit::uninit(); 65535];
        let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP)).unwrap();
        let address: SocketAddr = ADDRESS.parse().unwrap();

        let mut config =
            quiche::Config::new(quiche::PROTOCOL_VERSION).expect("error creating quiche config");

        let tls_key = std::path::absolute("../../pki/key.pem").unwrap();
        let tls_cert = std::path::absolute("../../pki/cert.pem").unwrap();

        config
            .load_priv_key_from_pem_file(&tls_key.to_string_lossy())
            .expect("couldn't load TLS key");

        config
            .load_cert_chain_from_pem_file(&tls_cert.to_string_lossy())
            .expect("couldn't load TLS cert");

        config.set_disable_active_migration(true);
        config.set_max_recv_udp_payload_size(MAX_DATAGRAM_SIZE);
        config.set_max_send_udp_payload_size(MAX_DATAGRAM_SIZE);
        config.set_max_idle_timeout(1000);
        config.set_initial_max_data(10000000);
        config.set_initial_max_stream_data_bidi_local(10000000);
        config.set_initial_max_stream_data_bidi_remote(1000000);
        config.set_initial_max_stream_data_uni(10000000);
        config.set_initial_max_streams_bidi(1000);
        config.set_initial_max_streams_uni(1000);
        config.enable_early_data();
        config.set_application_protos(RIDE_ALPN).unwrap();

        socket
            .set_reuse_port(true)
            .expect("failed to set SO_REUSEPORT");
        socket
            .set_nonblocking(true)
            .expect("failed to set O_NONBLOCK");

        socket.bind(&address.into()).expect("Failed to bind");

        let local_address = socket.local_addr().unwrap().as_socket().unwrap();

        let mut ev = epoll_event {
            events: EpollEvents::EPOLLIN as u32,
            data: epoll_data {
                fd: socket.as_raw_fd(),
            },
        };

        let epfd = unsafe {
            let fd = epoll_create1(0);
            if fd == -1 {
                perror(c"epoll_create1".as_ptr());
                process::exit(1);
            }
            fd
        };

        unsafe {
            if epoll_ctl(epfd, EPOLL_CTL_ADD, socket.as_raw_fd(), &mut ev) == -1 {
                perror(c"epoll_ctl".as_ptr());
            }

            ev.data.fd = sfd;
            if epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &mut ev) == -1 {
                perror(c"epoll_ctl".as_ptr());
            }
        }

        let clients = HashMap::new();
        let timeouts = BinaryHeap::new();

        Worker {
            socket,
            config,
            clients,
            buffer,
            timeouts,
            local_address,
            epfd,
            store,
            sfd,
        }
    }

    pub(crate) fn run(&mut self) {
        // Main event loop
        info!("Listening");
        'run: loop {
            let mut events: [epoll_event; MAXEVENTS] = unsafe { mem::zeroed() };

            // find CID that has the soonest expiry from now
            let timeout = match self.timeouts.peek() {
                Some(Reverse((expiry, _))) => expiry
                    .saturating_duration_since(Instant::now())
                    .as_millis()
                    .min(i32::MAX as u128) as i32,
                None => -1,
            };

            debug!("polling with timeout {}", timeout);
            let n = unsafe {
                let n = epoll_wait(self.epfd, events.as_mut_ptr(), 10, timeout);
                if n == -1 {
                    perror(c"epoll_wait".as_ptr());
                    process::exit(1);
                }
                n
            };
            debug!("polling returned {} events", n);

            // handle timeouts
            loop {
                trace!("checking timers. timers count: {}", self.timeouts.len());
                let expired = matches!(self.timeouts.peek(), Some(Reverse((expiry, cid))) if *expiry < Instant::now());
                if !expired {
                    trace!("no timeouts have occurred");
                    break;
                }

                // call timeout handler and re-arm timer
                let Reverse((expiry, cid)) = self.timeouts.pop().unwrap();
                if let Some(client) = self.clients.get_mut(&*cid) {
                    trace!(
                        "conn {:?} timed out. expiry={:?}, now={:?}, last_timeout={:?}",
                        client.conn.trace_id(),
                        expiry,
                        Instant::now(),
                        client.last_timeout,
                    );

                    if let Some(last_timeout) = client.last_timeout
                        && last_timeout >= expiry
                    {
                        trace!(
                            "conn {:?} already processed a later timeout={:?} skipping",
                            client.conn.trace_id(),
                            last_timeout
                        );
                        continue;
                    }

                    debug!("conn {:?} timeout", client.conn.trace_id(),);
                    client.conn.on_timeout();
                    client.last_timeout = Some(expiry);

                    if let Some(expiry) = client.conn.timeout_instant() {
                        trace!(
                            "conn {:?} re-arm timer to expiry={:?}",
                            client.conn.trace_id(),
                            expiry,
                        );
                        self.timeouts.push(Reverse((expiry, cid)));
                    } else {
                        trace!("conn {:?} disarm timer", client.conn.trace_id());
                    }
                }
            }

            // process receives if any
            if n > 0 {
                if unsafe { events[0].data.fd } == self.sfd {
                    // signalfd event (e.g. sigint caught)
                    break 'run;
                }

                trace!("processing receives");
                'read: loop {
                    match self.socket.recv_from(&mut self.buffer) {
                        Ok(v) => {
                            debug!("received {} bytes", v.0);
                            if !self.on_recv(v.0, v.1) {
                                continue 'read;
                            }
                        }
                        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                        Err(e) => {
                            error!("Error: {e}");
                            continue;
                        }
                    };
                }
            } else {
                debug!("no receive events");
            }

            // process sends
            for client in self.clients.values_mut() {
                trace!("conn {:?} processing sends", client.conn.trace_id());
                loop {
                    let conn = &mut client.conn;
                    let mut send_buf = unsafe { self.buffer.assume_init_mut() };
                    let (len, send_info) = match conn.send(&mut send_buf) {
                        Ok(v) => v,
                        Err(quiche::Error::Done) => break,
                        Err(e) => {
                            error!("Error {}", e);
                            continue;
                        }
                    };

                    self.socket
                        .send_to(&send_buf[..len], &send_info.to.into())
                        .expect("failed :(");

                    trace!("conn {:?} sent {} bytes", conn.trace_id(), len);
                }

                debug!(
                    "conn {:?} stats {:?}",
                    client.conn.trace_id(),
                    client.conn.stats()
                );
            }

            // drop closed connections
            self.clients.retain(|_, client| {
                if client.conn.is_closed() {
                    info!(
                        "conn {:?} dropped {:?}",
                        client.conn.trace_id(),
                        client.conn.stats(),
                    );
                }
                !client.conn.is_closed()
            });
        }
    }

    /// Processes a single receive
    /// If connection ID is new, add it,  neogitate version if needed and add to client map
    fn on_recv(&mut self, len: usize, from: SockAddr) -> bool {
        // read header to determine if new conn
        let pkt = unsafe { self.buffer[..len].assume_init_mut() };

        //TODO: generate proper conn ids
        let hdr = quiche::Header::from_slice(pkt, 16).expect("invalid header");

        let dcid: Rc<[u8]> = Rc::from(hdr.dcid.as_ref());
        trace!("received hdr: {:?}", hdr);

        let ride_client = if !self.clients.contains_key(&dcid) {
            if !quiche::version_is_supported(hdr.version) {
                warn!(
                    "WARNING: need to do version negotiaion, got version: {}",
                    hdr.version
                );

                let len = quiche::negotiate_version(&hdr.scid, &hdr.dcid, unsafe {
                    self.buffer.assume_init_mut()
                })
                .unwrap();

                self.socket
                    .send_to(unsafe { self.buffer[..len].assume_init_mut() }, &from)
                    .expect("failed sending neogitation packet");
                debug!("Sent negotiate packet");
                return false;
            }

            let conn = quiche::accept(
                &hdr.dcid,
                None,
                self.local_address,
                from.as_socket().unwrap(),
                &mut self.config,
            )
            .expect("error accepting conn");

            info!("conn {:?} accepted", conn.trace_id());
            self.clients.insert(
                dcid.clone(),
                RideClient {
                    conn,
                    last_timeout: None,
                },
            );
            self.clients.get_mut(&dcid).unwrap()
        } else {
            let conn = self.clients.get_mut(&dcid).unwrap();
            trace!("conn {:?} known", conn.conn.trace_id());
            conn
        };

        let recv_info = RecvInfo {
            from: from.as_socket().unwrap(),
            to: self.local_address,
        };

        trace!(
            "conn {:?} processing packet with header type: {:?}",
            ride_client.conn.trace_id(),
            hdr.ty
        );

        ride_client
            .conn
            .recv(pkt, recv_info)
            .expect("failed ingesting recv packet");

        if ride_client.conn.is_in_early_data() || ride_client.conn.is_established() {
            trace!("conn {:?} checking streams", ride_client.conn.trace_id());
            let buf = unsafe { self.buffer.assume_init_mut() };
            for s in ride_client.conn.readable() {
                while let Ok((n, _fin)) = ride_client.conn.stream_recv(s, buf) {
                    debug!("received {} bytes", n);
                    let m: Message = buf.try_into().expect("error parsing message");
                    if let Some(h) = self.store.get(&m.path) {
                        if *h == m.hash {
                            info!("verified correct hash for file {}", m.path)
                        } else {
                            warn!("incorrect hash received for file {}", m.path)
                        }
                    } else {
                        info!("learned new file {}", m.path);
                        self.store.add(m.path, m.hash);
                    }
                }
            }
        } else {
            trace!("conn {:?} is not established", ride_client.conn.trace_id());
        }

        if let Some(instant) = ride_client.conn.timeout_instant() {
            debug!(
                "conn {:?} arm timer, expiry={:?}",
                ride_client.conn.trace_id(),
                instant
            );
            self.timeouts.push(Reverse((instant, dcid)));
        }
        true
    }
}

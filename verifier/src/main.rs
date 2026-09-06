use quiche::RecvInfo;
use socket2::{Domain, Protocol, SockAddr, Socket, Type};
use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap};
use std::ffi::{c_char, c_int, c_void};
use std::mem::{self, MaybeUninit};
use std::net::SocketAddr;
use std::os::fd::AsRawFd;
use std::process::exit;
use std::rc::Rc;
use std::thread;
use std::time::Instant;

const ADDRESS: &str = "127.0.0.1:4433";
const RIDE_ALPN: &[&[u8]] = &[b"ride0.1"];
const MAXEVENTS: usize = 10;

struct RideClient {
    conn: quiche::Connection,
}

// -------------------------------------
// epoll ffi api
//TODO use safe wrapper, e.g. epoll-rs
#[repr(C)]
enum EpollEvents {
    EPOLLIN = 0x1,
}

const EPOLL_CTL_ADD: i32 = 1;

#[repr(C)]
#[derive(Copy, Clone)]
union epoll_data {
    ptr: *mut c_void,
    fd: c_int,
    u32: u32,
    u63: u64,
}

#[repr(C, packed)]
#[derive(Copy, Clone)]
pub struct epoll_event {
    events: u32,
    data: epoll_data,
}

unsafe extern "C" {
    pub fn epoll_create1(flags: c_int) -> c_int;
    pub fn epoll_wait(
        epfd: c_int,
        events: *mut epoll_event,
        maxevents: c_int,
        timeout: c_int,
    ) -> c_int;
    pub fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, event: *mut epoll_event) -> c_int;
    pub fn perror(s: *const c_char) -> c_void;
}
// -------------------------------------

struct Worker {
    buffer: [MaybeUninit<u8>; 65535],
    socket: Socket,
    clients: HashMap<Rc<[u8]>, RideClient>,
    config: quiche::Config,
    local_address: SocketAddr,
    timeouts: BinaryHeap<Reverse<(Instant, Rc<[u8]>)>>,
    epfd: c_int,
}

// Each worker thread binds to the same UDP host:port but with REUSE_PORT enabled
// in additional to parallelism offered by worker threads, each thread is able
// to concurrently handly multiple clients via epoll
impl Worker {
    fn new() -> Self {
        let buffer: [MaybeUninit<u8>; 65535] = [MaybeUninit::uninit(); 65535];
        let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP)).unwrap();
        let address: SocketAddr = ADDRESS.parse().unwrap();

        let mut config =
            quiche::Config::new(quiche::PROTOCOL_VERSION).expect("error creating quiche config");

        let tls_key = std::path::absolute("../pki/key.pem").unwrap();
        let tls_cert = std::path::absolute("../pki/cert.pem").unwrap();

        config
            .load_priv_key_from_pem_file(&tls_key.to_string_lossy())
            .expect("couldn't load TLS key");

        config
            .load_cert_chain_from_pem_file(&tls_cert.to_string_lossy())
            .expect("couldn't load TLS cert");

        config.set_disable_active_migration(false);

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
                exit(1);
            }
            fd
        };

        unsafe {
            if epoll_ctl(epfd, EPOLL_CTL_ADD, socket.as_raw_fd(), &mut ev) == -1 {
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
        }
    }

    fn run(&mut self) {
        // main event loop
        println!("Listening");
        loop {
            let mut events: [epoll_event; MAXEVENTS] = unsafe { mem::zeroed() };

            // find CID that has the soonest expiry from now
            let timeout = match self.timeouts.peek() {
                Some(Reverse((expiry, _))) => expiry
                    .saturating_duration_since(Instant::now())
                    .as_millis()
                    .min(i32::MAX as u128) as i32,
                None => -1,
            };

            let n = unsafe {
                let n = epoll_wait(self.epfd, events.as_mut_ptr(), 10, timeout);
                if n == -1 {
                    perror(c"epoll_wait".as_ptr());
                    exit(1);
                }
                n
            };

            // handle deadlines
            loop {
                let expired = matches!(self.timeouts.peek(), Some(Reverse((expiry, cid))) if *expiry < Instant::now());
                if !expired {
                    break;
                }

                // call timeout handler and re-arm timer
                let Reverse((_, cid)) = self.timeouts.pop().unwrap();
                if let Some(client) = self.clients.get_mut(&*cid) {
                    client.conn.on_timeout();
                    if let Some(expiry) = client.conn.timeout_instant() {
                        self.timeouts.push(Reverse((expiry, cid)));
                    }
                }
            }

            if n == 0 {
                continue;
            }

            // process receives if any
            loop {
                match self.socket.recv_from(&mut self.buffer) {
                    Ok(v) => self.on_recv(v.0, v.1),
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                    Err(e) => {
                        println!("Error: {e}");
                        continue;
                    }
                };
            }

            // process sends
            println!("sending");
            for client in self.clients.values_mut() {
                let conn = &mut client.conn;
                let mut send_buf = unsafe { self.buffer.assume_init_mut() };
                let (len, send_info) = match conn.send(&mut send_buf) {
                    Ok(v) => v,
                    Err(quiche::Error::Done) => break,
                    Err(e) => {
                        println!("Error {}", e);
                        continue;
                    }
                };
                println!("sending {} bytes", len);

                self.socket
                    .send_to(&send_buf[..len], &send_info.to.into())
                    .expect("failed :(");
            }

            // drop closed connections
            self.clients.retain(|_, client| {
                if client.conn.is_closed() {
                    println!(
                        "dropping {} : {:?}",
                        client.conn.trace_id(),
                        client.conn.stats()
                    );
                }
                !client.conn.is_closed()
            });
        }
    }

    // process a single receive
    // if connection ID is new, add it to clients map
    fn on_recv(&mut self, len: usize, from: SockAddr) -> (usize, SockAddr) {
        // read header to determine if new conn
        let pkt = unsafe { self.buffer[..len].assume_init_mut() };
        let hdr = quiche::Header::from_slice(pkt, quiche::MAX_CONN_ID_LEN).expect("invalid header");

        let dcid: Rc<[u8]> = Rc::from(hdr.dcid.as_ref());
        let ride_client = self.clients.entry(dcid.clone()).or_insert_with(|| {
            let conn = quiche::accept(
                &hdr.dcid,
                None,
                self.local_address,
                from.as_socket().unwrap(),
                &mut self.config,
            )
            .expect("error accepting conn");

            println!("New connetion!");
            RideClient { conn }
        });

        let recv_info = RecvInfo {
            from: from.as_socket().unwrap(),
            to: self.local_address,
        };

        ride_client
            .conn
            .recv(pkt, recv_info)
            .expect("failed ingesting recv packet");

        if let Some(instant) = ride_client.conn.timeout_instant() {
            self.timeouts.push(Reverse((instant, dcid)));
        }
        (len, from)
    }
}

fn main() {
    thread::scope(|s| {
        for _ in 0..3 {
            s.spawn(|| {
                let mut w = Worker::new();
                w.run();
            });
        }
    });
}

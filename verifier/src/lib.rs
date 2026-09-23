use log::info;
use std::mem::MaybeUninit;
use std::path::PathBuf;
use std::process::exit;
use std::sync::Arc;
use std::time::Duration;
use std::{i32, mem, thread};
use crate::store::Store;
use crate::worker::Worker;
use crate::ffi::{EventFDFlags::EFD_NONBLOCK, *};

mod ffi;
mod store;
mod worker;

/// How long to wait to allow for more writes in single batch
pub const DEFAULT_FLUSH_DELAY: Duration = Duration::from_secs(1);

/// Maximum size of write batch
pub const FLUSH_BATCH_SIZE: usize = 10;

/// Represents a Verifier instance.
///
/// This can be built using VerifierBuilder
pub struct Verifier {
    threads: usize,
    signal_fd: i32,
    db_path: PathBuf,
    flush_delay: Duration,
}

pub struct VerifierBuilder {
    threads: usize,
    signal_fd: i32,
    db_path: PathBuf,
    flush_delay: Duration,
}

impl Verifier {
    /// Returns a builder that can be used to build a Verifier instance.
    /// At bare-minimum, Verifier needs a properly constructed Linux `signalfd`
    /// that can allow it to monitor for interrupt signals and save db on exit.
    ///
    /// The reason for this design is that RIDE aims to be as minimalist as possible as far as
    /// library usage and currently only wants to support Linux.
    ///
    /// Builder also optionally allows to specify number of threads (default to number of cpus)
    /// and flush delay period which specifies how long to wait for more writes before flushing
    ///
    /// # Examples
    ///
    /// ```
    /// use libc:*;
    /// let mut mask = MaybeUninit::<libc::sigset_t>::uninit();
    /// let sfd;
    /// unsafe {
    ///   let mask = mask.as_mut_ptr();
    ///   libc::sigemptyset(mask);
    ///   llibc::pthread_sigmask(
    ///       libc::SIG_BLOCK,
    ///       mask,
    ///       std::ptr::null::<libc::sigset_t>() as *mut libc::sigset_t);
    ///   sfd = libc::signalfd(-1, mask, libc::SFD_NONBLOCK);
    /// }
    ///
    ///  Verifier::builder(sfd, PathBuf::from("./store.db")).threads(4).run();
    /// ```
    pub fn builder(signal_fd: i32, db_path: PathBuf) -> VerifierBuilder {
        VerifierBuilder::new(signal_fd, db_path)
    }

    pub fn run(self) {
        // create an event_fd to use for communicating with store
        // this will be used to notify main thread when it's ready to save
        let event_fd = unsafe { eventfd(0, EFD_NONBLOCK as i32) };
        if event_fd == -1 {
            unsafe {
                perror(c"eventfd".as_ptr());
            }
            exit(1);
        }

        let store = Arc::new(Store::new(self.db_path, event_fd));

        // spawn server threds
        for _ in 0..self.threads {
            let st = store.clone();
            thread::spawn(move || {
                let mut w = Worker::new(&st, self.signal_fd);
                w.run();
            });
        }

        // epoll loop to monitor signal fd (for interrupts) and event fd (for new store changes)
        let epfd = Self::setup_epoll(self.signal_fd, event_fd);
        let mut timeout = -1;
        let mut pending = 0;
        let mut events: [epoll_event; 1] = unsafe { mem::zeroed() };
        let mut _read = MaybeUninit::new(0);
        loop {
            let n = unsafe { epoll_wait(epfd, events.as_mut_ptr(), 1, timeout) };
            if n > 0 && unsafe { events[0].data.fd } == self.signal_fd {
                log::info!("caught signal");
                break;
            } else if n > 0 && unsafe { events[0].data.fd } == event_fd {
                // set timeout to flush delay and save on next timeout
                // this allows us to batch writes
                pending = pending + 1;
                if pending < FLUSH_BATCH_SIZE {
                    timeout = self.flush_delay.as_millis().clamp(1, i32::MAX as u128) as i32;
                    continue;
                }
            } else if n == 0 {
                info!(
                    "did not receive write events in {:?}, saving current db batch",
                    self.flush_delay
                );
            }
            unsafe {
                eventfd_read(event_fd, _read.as_mut_ptr());
                println!("eventfd counter: {}", _read.assume_init());
            }
            info!("saving batch of size {}", unsafe { _read.assume_init() });
            store.save();
            pending = 0;
            timeout = -1;
        }
        store.save();
    }

    // creates an epoll and registers provided signal fd and event_fd in its set
    fn setup_epoll(signal_fd: i32, event_fd: i32) -> i32 {
        let epfd = unsafe { epoll_create1(0) };
        if epfd == -1 {}
        let mut epoll_event = epoll_event {
            events: EpollEvents::EPOLLIN as u32,
            data: epoll_data { fd: signal_fd },
        };

        // register signal fd
        if unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, signal_fd, &mut epoll_event) == -1 } {
            unsafe {
                perror(c"epoll_ctl".as_ptr());
            }
            exit(1);
        }

        // register event fd
        epoll_event.data.fd = event_fd;
        epoll_event.events = EpollEvents::EPOLLET as u32 | EpollEvents::EPOLLIN as u32;
        if unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &mut epoll_event) == -1 } {
            unsafe {
                perror(c"epoll_ctl".as_ptr());
            }
            exit(1);
        }

        epfd
    }
}

impl VerifierBuilder {
    /// VierifierBuilder allows you to create Verifier instances.
    ///
    /// See examples in [builder](Verifier).
    pub fn new(signal_fd: i32, db_path: PathBuf) -> VerifierBuilder {
        let ncpus = std::thread::available_parallelism()
            .expect("could not determine number CPUs, consider passing it as parameter");

        VerifierBuilder {
            threads: ncpus.get(),
            signal_fd: signal_fd,
            flush_delay: DEFAULT_FLUSH_DELAY,
            db_path,
        }
    }

    /// Number of verifier threads to spawn. Each thread will independently accept UDP QUIC
    /// connections (on the same port, via kernel multiplexer, i.e. SO_REUSEPORT). Furthermore,
    /// verifier threads are able to verify concurrent hashes via an async event loop.
    /// Defaults to number of cores in the system.
    pub fn threads(mut self, nthreads: usize) -> VerifierBuilder {
        self.threads = nthreads;
        self
    }

    /// How long to wait for more updates to store.db before flushing (write coalescing)
    /// Defauilts to [DEFAULT_FLUSH_DELAY](self)
    pub fn flush_delay(mut self, flush_delay: Duration) -> VerifierBuilder {
        self.flush_delay = flush_delay;
        self
    }

    /// Returns a Verifier instance. You can invoke run() to actually run the Verifier
    pub fn build(self) -> Verifier {
        Verifier {
            threads: self.threads,
            signal_fd: self.signal_fd,
            flush_delay: self.flush_delay,
            db_path: self.db_path,
        }
    }
}

use clap::Parser;
use std::mem::MaybeUninit;
use std::path::PathBuf;
use std::process::exit;
use verifier::Verifier;

/// A remote verifier for RIDE
#[derive(Parser, Debug)]
#[command(version, about, long_about = None)]
struct Args {
    /// Number of server threads (defaults to number of cores)
    #[arg(short, long)]
    threads: Option<usize>,

    /// Location of verifier db (will create new one if it doesn't exist)
    #[arg(short, long, default_value_t = "./store.db".to_string())]
    db: String,
}

fn main() {
    let args = Args::parse();

    unsafe {
        std::env::set_var("RUST_LOG", "info,quiche=info");
    };
    env_logger::init();

    // setup signalfd to use for interrupt handling
    let sfd = setup_signals();

    // create canocnical db path
    let db_path = std::path::absolute(PathBuf::from(args.db))
        .expect("could not construct canocnical db path");

    // build Verifier and run it
    let mut b = Verifier::builder(sfd, db_path);
    if let Some(t) = args.threads {
        b = b.threads(t);
    }
    b.build().run();
}

/// Creates signalfd and add typical interrupt signals
/// to its sets. Also blocks typical handler of such signals
/// The return signal fd can be used to watch for interrupt signals
fn setup_signals() -> i32 {
    let mut mask = MaybeUninit::<libc::sigset_t>::uninit();
    let sfd;
    unsafe {
        let mask = mask.as_mut_ptr();
        libc::sigemptyset(mask);
        libc::sigaddset(mask, libc::SIGINT);
        libc::sigaddset(mask, libc::SIGTERM);
        libc::sigaddset(mask, libc::SIGHUP);
        libc::sigaddset(mask, libc::SIGQUIT);
        if libc::pthread_sigmask(
            libc::SIG_BLOCK,
            mask,
            std::ptr::null::<libc::sigset_t>() as *mut libc::sigset_t,
        ) == -1
        {
            libc::perror(c"sigprocmask".as_ptr());
            exit(1);
        }
        sfd = libc::signalfd(-1, mask, libc::SFD_NONBLOCK);
        if sfd < 0 {
            libc::perror(c"signalfd".as_ptr());
            exit(1);
        }
    }
    sfd
}

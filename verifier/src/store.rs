use crate::ffi::*;
use dashmap::{DashMap, mapref::one::Ref};
use log::info;
use std::{fs::File, io::ErrorKind::NotFound, path::PathBuf};

pub(crate) const HASH_LEN: usize = 32;

type HashesMap = DashMap<String, [u8; HASH_LEN]>;

pub(crate) struct Store {
    hashes: HashesMap,
    db_path: PathBuf,
    event_fd: i32,
}

impl Store {
    pub(crate) fn new(db_path: PathBuf, event_fd: i32) -> Self {
        let hashes = match File::open(&db_path) {
            Ok(f) => {
                let hashes: HashesMap = serde_json::from_reader(&f).expect("failed to parse db");
                info!("loaded {:?} with {} entries", &db_path, hashes.len());
                hashes
            }
            Err(e) if e.kind() == NotFound => {
                info!("creating new db at {:?}", &db_path);
                DashMap::new()
            }

            Err(e) => panic!("Error opening store.db: {}", e),
        };

        Store {
            hashes,
            db_path,
            event_fd,
        }
    }

    pub(crate) fn add(&self, file: String, hash: [u8; HASH_LEN]) {
        self.hashes.insert(file, hash);

        let ret = unsafe { eventfd_write(self.event_fd, 1) };
        if ret == -1 {
            unsafe {
                perror(c"eventfd_write".as_ptr());
            }
        }
    }

    pub(crate) fn get(&self, file: &String) -> Option<Ref<'_, String, [u8; HASH_LEN]>> {
        self.hashes.get(file)
    }

    pub(crate) fn save(&self) {
        log::info!("saving db");

        match File::options().create(true).write(true).open(&self.db_path) {
            Ok(f) => serde_json::to_writer(f, &self.hashes).expect("failed to save db"),
            Err(e) => log::error!("error saving db: {}", e),
        };
    }
}

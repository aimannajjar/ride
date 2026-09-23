//! Why POSIX FFI? Because RIDE aims to be as minimalist as possible (as to reduce surface risk)
//! and currently we're focusing on Linux. Over time, 3rd-party crates can be adapted to replace
//! functinality provided here.
use std::ffi::{c_char, c_int, c_uint, c_void};

#[repr(C)]
pub(crate) enum EpollEvents {
    EPOLLIN = 0x1,
    EPOLLET = 1 << 31,
}

pub(crate) const EPOLL_CTL_ADD: i32 = 1;

#[repr(C)]
#[allow(non_camel_case_types, unused)]
pub(crate) enum EventFDFlags {
    EFD_SEMAPHORE = 0o00000001,
    EFD_CLOEXEC = 0o02000000,
    EFD_NONBLOCK = 0o00004000,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) union epoll_data {
    pub(crate) ptr: *mut c_void,
    pub(crate) fd: c_int,
    pub(crate) u32: u32,
    pub(crate) u63: u64,
}

#[repr(C, packed)]
#[derive(Copy, Clone)]
pub(crate) struct epoll_event {
    pub(crate) events: u32,
    pub(crate) data: epoll_data,
}

// TODO: Create safe wrappers
unsafe extern "C" {
    pub(crate) fn epoll_create1(flags: c_int) -> c_int;
    pub(crate) fn epoll_wait(
        epfd: c_int,
        events: *mut epoll_event,
        maxevents: c_int,
        timeout: c_int,
    ) -> c_int;
    pub(crate) fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, event: *mut epoll_event) -> c_int;
    pub(crate) fn perror(s: *const c_char) -> c_void;
    pub(crate) fn eventfd(initval: c_uint, flags: c_int) -> c_int;
    pub(crate) fn eventfd_read(fd: c_int, val: *mut u64) -> c_int;
    pub(crate) fn eventfd_write(fd: c_int, val: u64) -> c_int;
}

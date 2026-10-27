// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg(target_os = "android")]
//! Rust replacement for the manager's former C++ JNI layer.

use std::ffi::{CStr, CString};
use std::fs;
use std::os::fd::RawFd;
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Mutex, Once};

use jni::EnvUnowned;
use jni::errors::ThrowRuntimeExAndDefault;
use jni::objects::{JObject, JString};
use jni::sys::{JNI_FALSE, JNI_TRUE, JNI_VERSION_1_6, jboolean, jbyteArray, jint, jlong, jstring};

const FMAC_MAX_DATA: usize = 64 * 1024;
const FMAC_STACK_DATA: usize = 264;
const FMAC_DATA_CAP: usize = 12;
const FMAC_DATA_SELRULE: usize = 264;
const FMAC_DATA_PROFILE: usize = 84;
const FMAC_DATA_FEATURE: usize = 12;
const FMAC_OFF_UID: usize = 0;
const FMAC_OFF_CAPS: usize = 4;
const FMAC_OFF_DOMAIN: usize = 12;
const FMAC_OFF_NS: usize = 76;
const FMAC_OFF_ROOT_UID: usize = 80;
const FMAC_OFF_TGT: usize = 64;
const FMAC_OFF_CLS: usize = 128;
const FMAC_OFF_PERM: usize = 192;
const FMAC_OFF_EFFECT: usize = 256;
const FMAC_OFF_INVERT: usize = 260;
const FMAC_OFF_FEATURE_VALUE: usize = 4;

const IOC_CMD: libc::c_int = 0xC008_4600u32 as libc::c_int;
const IOC_ADD_UID: u32 = 3;
const IOC_DEL_UID: u32 = 4;
const IOC_HAS_UID: u32 = 5;
const IOC_SET_CAP: u32 = 6;
const IOC_GET_CAP: u32 = 7;
const IOC_DEL_CAP: u32 = 8;
const IOC_SEL_ADD_RULE: u32 = 9;
const IOC_SET_PROFILE: u32 = 10;
const IOC_GET_PROFILES: u32 = 13;
const IOC_GET_VERSION: u32 = 14;
const IOC_FEATURE_LIST: u32 = 15;
const IOC_FEATURE_GET: u32 = 16;
const IOC_FEATURE_SET: u32 = 17;

const PR_GET_SHM_FD: libc::c_int = 201;
const PR_GET_CTL_FD: libc::c_int = 203;

const READER_MAX_PATH: usize = 4096;
const READER_CHUNK: usize = 64 * 1024;

static DRIVER_FD: AtomicI32 = AtomicI32::new(-1);
static CTL_FD: AtomicI32 = AtomicI32::new(-1);

fn scan_fd(target: &str) -> RawFd {
    let Ok(entries) = fs::read_dir("/proc/self/fd") else {
        return -1;
    };
    for entry in entries.flatten() {
        let Ok(fd) = entry.file_name().to_string_lossy().parse::<i32>() else {
            continue;
        };
        let link = format!("/proc/self/fd/{fd}");
        if let Ok(path) = fs::read_link(link)
            && path.to_string_lossy().contains(target)
        {
            return fd;
        }
    }
    -1
}

fn ensure_ctl_fd() -> Option<RawFd> {
    let cached = CTL_FD.load(Ordering::Relaxed);
    if cached >= 0 {
        return Some(cached);
    }
    unsafe {
        libc::prctl(PR_GET_CTL_FD, 0, 0, 0, 0);
    }
    let fd = scan_fd("[fmac_ctl]");
    if fd >= 0 {
        CTL_FD.store(fd, Ordering::Relaxed);
        Some(fd)
    } else {
        log::error!("control fd not found after prctl({PR_GET_CTL_FD})");
        None
    }
}

fn ioc_call(fd: RawFd, flag: u32, data: &mut [u8]) -> i32 {
    if data.len() > FMAC_MAX_DATA {
        return -1;
    }
    let total = 8 + data.len();
    let mut stack = [0u8; 8 + FMAC_STACK_DATA];
    let mut heap = Vec::new();
    let buffer: &mut [u8] = if total <= stack.len() {
        &mut stack[..total]
    } else {
        heap.resize(total, 0);
        &mut heap
    };
    buffer[0..4].copy_from_slice(&flag.to_ne_bytes());
    buffer[4..8].copy_from_slice(&(data.len() as u32).to_ne_bytes());
    buffer[8..].copy_from_slice(data);

    let ret = unsafe { libc::ioctl(fd, IOC_CMD, buffer.as_mut_ptr()) };
    if ret < 0 {
        log::error!(
            "ioctl flag {flag} failed: {}",
            std::io::Error::last_os_error()
        );
    } else if !data.is_empty() {
        data.copy_from_slice(&buffer[8..]);
    }
    ret
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_ctl(
    _env: EnvUnowned,
    _this: JObject,
    value: jint,
) -> jint {
    log::info!("ctl({value})");
    match value {
        1 => {
            unsafe {
                libc::prctl(PR_GET_SHM_FD, 0, 0, 0, 0);
            }
            let fd = scan_fd("[fmac_shm]");
            if fd >= 0 {
                DRIVER_FD.store(fd, Ordering::Relaxed);
            }
        }
        3 => {
            ensure_ctl_fd();
        }
        _ => return -1,
    }
    if DRIVER_FD.load(Ordering::Relaxed) >= 0 {
        0
    } else {
        -1
    }
}

fn get_utf(env: &mut EnvUnowned, value: &JString) -> Option<String> {
    if value.as_raw().is_null() {
        None
    } else {
        env.with_env(|env| Ok::<_, jni::errors::Error>(value.try_to_string(env).ok()))
            .resolve::<ThrowRuntimeExAndDefault>()
    }
}

fn put_string(env: &mut EnvUnowned, value: &str) -> jstring {
    env.with_env(|env| {
        Ok::<_, jni::errors::Error>(
            env.new_string(value)
                .map(|s| s.into_raw())
                .unwrap_or(ptr::null_mut()),
        )
    })
    .resolve::<ThrowRuntimeExAndDefault>()
}

fn put_bytes(env: &mut EnvUnowned, data: &[u8]) -> jbyteArray {
    env.with_env(|env| {
        Ok::<_, jni::errors::Error>(
            env.byte_array_from_slice(data)
                .map(|a| a.into_raw())
                .unwrap_or(ptr::null_mut()),
        )
    })
    .resolve::<ThrowRuntimeExAndDefault>()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_setProfile(
    mut env: EnvUnowned,
    _this: JObject,
    uid: jint,
    root_uid: jint,
    caps: jlong,
    domain: JString,
    ns: jint,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    let domain = get_utf(&mut env, &domain).unwrap_or_default();
    let mut data = [0u8; FMAC_DATA_PROFILE];
    data[FMAC_OFF_UID..FMAC_OFF_UID + 4].copy_from_slice(&(uid as u32).to_ne_bytes());
    data[FMAC_OFF_CAPS..FMAC_OFF_CAPS + 8].copy_from_slice(&(caps as u64).to_ne_bytes());
    let bytes = domain.as_bytes();
    let len = bytes.len().min(FMAC_OFF_NS - FMAC_OFF_DOMAIN - 1);
    data[FMAC_OFF_DOMAIN..FMAC_OFF_DOMAIN + len].copy_from_slice(&bytes[..len]);
    data[FMAC_OFF_NS..FMAC_OFF_NS + 4].copy_from_slice(&ns.to_ne_bytes());
    data[FMAC_OFF_ROOT_UID..FMAC_OFF_ROOT_UID + 4]
        .copy_from_slice(&(root_uid as u32).to_ne_bytes());
    if ioc_call(fd, IOC_SET_PROFILE, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_adduid(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    if uid < 0 {
        return -1;
    }
    let mut data = (uid as u32).to_ne_bytes();
    if ioc_call(fd, IOC_ADD_UID, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_deluid(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    if uid < 0 {
        return -1;
    }
    let mut data = (uid as u32).to_ne_bytes();
    if ioc_call(fd, IOC_DEL_UID, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_hasuid(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    if uid < 0 {
        return -1;
    }
    let mut data = (uid as u32).to_ne_bytes();
    if ioc_call(fd, IOC_HAS_UID, &mut data) < 0 {
        return -1;
    }
    if u32::from_ne_bytes(data) != 0 { 1 } else { 0 }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_setCap(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
    caps: jlong,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    if uid < 0 {
        return -1;
    }
    let mut data = [0u8; FMAC_DATA_CAP];
    data[FMAC_OFF_UID..FMAC_OFF_UID + 4].copy_from_slice(&(uid as u32).to_ne_bytes());
    data[FMAC_OFF_CAPS..FMAC_OFF_CAPS + 8].copy_from_slice(&(caps as u64).to_ne_bytes());
    if ioc_call(fd, IOC_SET_CAP, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_getCap(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jlong {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    if uid < 0 {
        return -1;
    }
    let mut data = [0u8; FMAC_DATA_CAP];
    data[FMAC_OFF_UID..FMAC_OFF_UID + 4].copy_from_slice(&(uid as u32).to_ne_bytes());
    if ioc_call(fd, IOC_GET_CAP, &mut data) < 0 {
        return -1;
    }
    u64::from_ne_bytes(data[FMAC_OFF_CAPS..FMAC_OFF_CAPS + 8].try_into().unwrap()) as jlong
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_delCap(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    if uid < 0 {
        return -1;
    }
    let mut data = [0u8; FMAC_DATA_CAP];
    data[FMAC_OFF_UID..FMAC_OFF_UID + 4].copy_from_slice(&(uid as u32).to_ne_bytes());
    if ioc_call(fd, IOC_DEL_CAP, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_addSelinuxRule(
    mut env: EnvUnowned,
    _this: JObject,
    src: JString,
    tgt: JString,
    cls: JString,
    perm: JString,
    effect: jint,
    invert: jboolean,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    let src = get_utf(&mut env, &src).unwrap_or_default();
    let tgt = get_utf(&mut env, &tgt).unwrap_or_default();
    let cls = get_utf(&mut env, &cls).unwrap_or_default();
    let perm = get_utf(&mut env, &perm).unwrap_or_default();
    let mut data = [0u8; FMAC_DATA_SELRULE];
    let mut put = |offset: usize, cap: usize, value: &str| {
        let bytes = value.as_bytes();
        let len = bytes.len().min(cap - 1);
        data[offset..offset + len].copy_from_slice(&bytes[..len]);
    };
    put(FMAC_OFF_UID, FMAC_OFF_TGT - FMAC_OFF_UID, &src);
    put(FMAC_OFF_TGT, FMAC_OFF_CLS - FMAC_OFF_TGT, &tgt);
    put(FMAC_OFF_CLS, FMAC_OFF_PERM - FMAC_OFF_CLS, &cls);
    put(FMAC_OFF_PERM, FMAC_OFF_EFFECT - FMAC_OFF_PERM, &perm);
    data[FMAC_OFF_EFFECT..FMAC_OFF_EFFECT + 4].copy_from_slice(&effect.to_ne_bytes());
    let inv: i32 = if invert { 1 } else { 0 };
    data[FMAC_OFF_INVERT..FMAC_OFF_INVERT + 4].copy_from_slice(&inv.to_ne_bytes());
    if ioc_call(fd, IOC_SEL_ADD_RULE, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_addRule(
    _env: EnvUnowned,
    _this: JObject,
    _path: JString,
    _bits: jlong,
) -> jint {
    0
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_delRule(
    _env: EnvUnowned,
    _this: JObject,
    _path: JString,
) -> jint {
    0
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_listProfiles(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    let Some(fd) = ensure_ctl_fd() else {
        return ptr::null_mut();
    };
    let mut data = vec![0u8; 64 * 1024];
    let ret = ioc_call(fd, IOC_GET_PROFILES, &mut data);
    if ret < 0 {
        log::error!("listProfiles failed: ret={ret}");
        return ptr::null_mut();
    }
    let end = data.iter().position(|b| *b == 0).unwrap_or(data.len());
    log::info!("listProfiles: ret={ret}, text_len={end}");
    put_string(&mut env, &String::from_utf8_lossy(&data[..end]))
}

fn write_all(fd: RawFd, mut buf: &[u8]) -> bool {
    while !buf.is_empty() {
        let n = unsafe { libc::write(fd, buf.as_ptr().cast(), buf.len()) };
        if n > 0 {
            buf = &buf[n as usize..];
        } else if n < 0 && unsafe { *libc::__errno() } == libc::EINTR {
            continue;
        } else {
            return false;
        }
    }
    true
}

fn read_exact_fd(fd: RawFd, mut buf: &mut [u8]) -> bool {
    while !buf.is_empty() {
        let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
        if n > 0 {
            buf = &mut buf[n as usize..];
        } else if n == 0 {
            return false;
        } else if unsafe { *libc::__errno() } == libc::EINTR {
            continue;
        } else {
            return false;
        }
    }
    true
}

fn read_fd_all(fd: RawFd) -> Option<Vec<u8>> {
    let mut data = Vec::new();
    loop {
        data.reserve(READER_CHUNK);
        let spare = data.spare_capacity_mut();
        let n = unsafe { libc::read(fd, spare.as_mut_ptr().cast(), READER_CHUNK) };
        if n > 0 {
            unsafe { data.set_len(data.len() + n as usize) };
        } else if n == 0 {
            break;
        } else if unsafe { *libc::__errno() } == libc::EINTR {
            continue;
        } else {
            return None;
        }
    }
    Some(data)
}

fn wait_child(pid: libc::pid_t) -> bool {
    let mut status = 0;
    loop {
        let ret = unsafe { libc::waitpid(pid, &mut status, 0) };
        if ret >= 0 {
            break;
        }
        if unsafe { *libc::__errno() } != libc::EINTR {
            return false;
        }
    }
    libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0
}

/*
 * Persistent root reader. One child serves [u32 len][path] -> [u32 status]
 * -> [u32 len][bytes]... [u32 0], using only async-signal-safe calls after
 * fork so the multithreaded manager stays safe.
 */
#[derive(Clone, Copy)]
struct RootReader {
    pid: libc::pid_t,
    req_fd: RawFd,
    resp_fd: RawFd,
}

impl RootReader {
    const fn new() -> Self {
        Self {
            pid: -1,
            req_fd: -1,
            resp_fd: -1,
        }
    }
}

static ROOT_READER: Mutex<RootReader> = Mutex::new(RootReader::new());
static IGNORE_SIGPIPE: Once = Once::new();

fn ignore_sigpipe() {
    IGNORE_SIGPIPE.call_once(|| unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    });
}

unsafe fn reader_child(req: RawFd, resp: RawFd) -> ! {
    if unsafe { libc::setresuid(0, 0, 0) } != 0 || unsafe { libc::getuid() } != 0 {
        unsafe { libc::_exit(1) };
    }

    let mut path = [0u8; READER_MAX_PATH];
    let mut buffer = [0u8; READER_CHUNK];
    loop {
        let mut plen_buf = [0u8; 4];
        if !read_exact_fd(req, &mut plen_buf) {
            unsafe { libc::_exit(0) };
        }
        let plen = u32::from_ne_bytes(plen_buf) as usize;
        if plen == 0 || plen >= READER_MAX_PATH || !read_exact_fd(req, &mut path[..plen]) {
            unsafe { libc::_exit(0) };
        }
        path[plen] = 0;

        let file = unsafe {
            libc::open(
                path.as_ptr().cast(),
                libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NONBLOCK,
            )
        };
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        let regular = file >= 0
            && unsafe { libc::fstat(file, &mut stat) } == 0
            && stat.st_mode & libc::S_IFMT == libc::S_IFREG;
        if !regular {
            if file >= 0 {
                unsafe { libc::close(file) };
            }
            if !write_all(resp, &1u32.to_ne_bytes()) {
                unsafe { libc::_exit(2) };
            }
            continue;
        }

        if !write_all(resp, &0u32.to_ne_bytes()) {
            unsafe { libc::close(file) };
            unsafe { libc::_exit(2) };
        }
        loop {
            let n = unsafe { libc::read(file, buffer.as_mut_ptr().cast(), buffer.len()) };
            if n > 0 {
                let len = (n as u32).to_ne_bytes();
                if !write_all(resp, &len) || !write_all(resp, &buffer[..n as usize]) {
                    unsafe { libc::close(file) };
                    unsafe { libc::_exit(2) };
                }
            } else if n == 0 {
                break;
            } else if unsafe { *libc::__errno() } == libc::EINTR {
                continue;
            } else {
                break;
            }
        }
        unsafe { libc::close(file) };
        if !write_all(resp, &0u32.to_ne_bytes()) {
            unsafe { libc::_exit(2) };
        }
    }
}

fn reader_start_locked(reader: &mut RootReader) -> bool {
    let mut req = [0; 2];
    let mut resp = [0; 2];
    if unsafe { libc::pipe(req.as_mut_ptr()) } != 0 {
        return false;
    }
    if unsafe { libc::pipe(resp.as_mut_ptr()) } != 0 {
        unsafe {
            libc::close(req[0]);
            libc::close(req[1]);
        }
        return false;
    }

    let pid = unsafe { libc::fork() };
    if pid < 0 {
        unsafe {
            libc::close(req[0]);
            libc::close(req[1]);
            libc::close(resp[0]);
            libc::close(resp[1]);
        }
        return false;
    }

    if pid == 0 {
        unsafe {
            libc::close(req[1]);
            libc::close(resp[0]);
            reader_child(req[0], resp[1]);
        }
    }

    unsafe {
        libc::close(req[0]);
        libc::close(resp[1]);
    }
    *reader = RootReader {
        pid,
        req_fd: req[1],
        resp_fd: resp[0],
    };
    true
}

fn reader_drop_locked(reader: &mut RootReader) {
    if reader.req_fd >= 0 {
        unsafe { libc::close(reader.req_fd) };
    }
    if reader.resp_fd >= 0 {
        unsafe { libc::close(reader.resp_fd) };
    }
    if reader.pid > 0 {
        let _ = wait_child(reader.pid);
    }
    *reader = RootReader::new();
}

fn reader_read_locked(reader: &mut RootReader, path: &[u8]) -> Option<Vec<u8>> {
    if reader.pid <= 0 && !reader_start_locked(reader) {
        return None;
    }

    if !write_all(reader.req_fd, &(path.len() as u32).to_ne_bytes())
        || !write_all(reader.req_fd, path)
    {
        reader_drop_locked(reader);
        return None;
    }

    let mut status = [0u8; 4];
    if !read_exact_fd(reader.resp_fd, &mut status) {
        reader_drop_locked(reader);
        return None;
    }
    if u32::from_ne_bytes(status) != 0 {
        return Some(Vec::new());
    }

    let mut out = Vec::new();
    loop {
        let mut chunk_buf = [0u8; 4];
        if !read_exact_fd(reader.resp_fd, &mut chunk_buf) {
            reader_drop_locked(reader);
            return None;
        }
        let chunk = u32::from_ne_bytes(chunk_buf) as usize;
        if chunk == 0 {
            break;
        }
        if chunk > READER_CHUNK {
            reader_drop_locked(reader);
            return None;
        }
        let Some(new_len) = out.len().checked_add(chunk) else {
            reader_drop_locked(reader);
            return None;
        };
        out.resize(new_len, 0);
        if !read_exact_fd(reader.resp_fd, &mut out[new_len - chunk..]) {
            reader_drop_locked(reader);
            return None;
        }
    }
    Some(out)
}

fn read_file_persistent_impl(path: &str) -> Option<Vec<u8>> {
    let path = path.as_bytes();
    if path.is_empty() || path.len() >= READER_MAX_PATH || path.contains(&0) {
        return None;
    }

    ignore_sigpipe();
    let mut reader = match ROOT_READER.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };
    match reader_read_locked(&mut reader, path) {
        Some(data) => Some(data),
        None => reader_read_locked(&mut reader, path),
    }
}

fn read_file_impl(path: &str) -> Option<Vec<u8>> {
    let path = CString::new(path).ok()?;
    let mut pipe = [0; 2];
    if unsafe { libc::pipe(pipe.as_mut_ptr()) } != 0 {
        return None;
    }
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        unsafe {
            libc::close(pipe[0]);
            libc::close(pipe[1]);
        }
        return None;
    }
    if pid == 0 {
        unsafe {
            libc::close(pipe[0]);
            if libc::setresuid(0, 0, 0) != 0 || libc::getuid() != 0 {
                libc::_exit(1);
            }
            let file = libc::open(path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC);
            if file < 0 {
                libc::_exit(2);
            }
            let mut buffer = [0u8; 65536];
            loop {
                let n = libc::read(file, buffer.as_mut_ptr().cast(), buffer.len());
                if n > 0 {
                    let mut off = 0;
                    while off < n {
                        let w = libc::write(
                            pipe[1],
                            buffer.as_ptr().add(off as usize).cast(),
                            (n - off) as usize,
                        );
                        if w > 0 {
                            off += w;
                        } else if w < 0 && *libc::__errno() == libc::EINTR {
                            continue;
                        } else {
                            libc::_exit(3);
                        }
                    }
                } else if n == 0 {
                    break;
                } else if *libc::__errno() != libc::EINTR {
                    libc::_exit(4);
                }
            }
            libc::close(file);
            libc::close(pipe[1]);
            libc::_exit(0);
        }
    }

    unsafe {
        libc::close(pipe[1]);
    }
    let data = read_fd_all(pipe[0]);
    unsafe {
        libc::close(pipe[0]);
    }
    if wait_child(pid) { data } else { None }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_readFile(
    mut env: EnvUnowned,
    _this: JObject,
    path: JString,
) -> jbyteArray {
    let Some(path) = get_utf(&mut env, &path) else {
        return ptr::null_mut();
    };
    match read_file_impl(&path) {
        Some(data) if !data.is_empty() => put_bytes(&mut env, &data),
        _ => {
            log::warn!("readFile({path}) failed or empty");
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_readFilePersistent(
    mut env: EnvUnowned,
    _this: JObject,
    path: JString,
) -> jbyteArray {
    let Some(path) = get_utf(&mut env, &path) else {
        return ptr::null_mut();
    };
    match read_file_persistent_impl(&path) {
        Some(data) => put_bytes(&mut env, &data),
        None => {
            log::warn!("readFilePersistent({path}) failed");
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_writeBlock(
    mut env: EnvUnowned,
    _this: JObject,
    src: JString,
    dst: JString,
) -> jboolean {
    let (Some(src), Some(dst)) = (get_utf(&mut env, &src), get_utf(&mut env, &dst)) else {
        return JNI_FALSE;
    };
    let (Ok(src), Ok(dst)) = (CString::new(src), CString::new(dst)) else {
        return JNI_FALSE;
    };
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        return JNI_FALSE;
    }
    if pid == 0 {
        unsafe {
            if libc::setresuid(0, 0, 0) != 0 || libc::getuid() != 0 {
                libc::_exit(1);
            }
            let input = libc::open(src.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC);
            if input < 0 {
                libc::_exit(2);
            }
            let output = libc::open(
                dst.as_ptr(),
                libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC | libc::O_CLOEXEC,
                0o644,
            );
            if output < 0 {
                libc::close(input);
                libc::_exit(3);
            }
            let mut src_stat: libc::stat = std::mem::zeroed();
            let mut dst_stat: libc::stat = std::mem::zeroed();
            if libc::fstat(input, &mut src_stat) != 0 || libc::fstat(output, &mut dst_stat) != 0 {
                libc::close(output);
                libc::close(input);
                libc::_exit(6);
            }
            if dst_stat.st_mode & libc::S_IFMT == libc::S_IFBLK
                && src_stat.st_mode & libc::S_IFMT == libc::S_IFREG
            {
                let capacity = libc::lseek(output, 0, libc::SEEK_END);
                if capacity > 0 && src_stat.st_size > capacity {
                    libc::close(output);
                    libc::close(input);
                    libc::_exit(7);
                }
                if libc::lseek(output, 0, libc::SEEK_SET) < 0 {
                    libc::close(output);
                    libc::close(input);
                    libc::_exit(8);
                }
            }
            let mut buffer = [0u8; 65536];
            loop {
                let n = libc::read(input, buffer.as_mut_ptr().cast(), buffer.len());
                if n > 0 {
                    let mut off = 0;
                    while off < n {
                        let w = libc::write(
                            output,
                            buffer.as_ptr().add(off as usize).cast(),
                            (n - off) as usize,
                        );
                        if w > 0 {
                            off += w;
                        } else if w < 0 && *libc::__errno() == libc::EINTR {
                            continue;
                        } else {
                            libc::close(output);
                            libc::close(input);
                            libc::_exit(4);
                        }
                    }
                } else if n == 0 {
                    break;
                } else if *libc::__errno() != libc::EINTR {
                    libc::close(output);
                    libc::close(input);
                    libc::_exit(5);
                }
            }
            libc::fsync(output);
            libc::close(output);
            libc::close(input);
            libc::_exit(0);
        }
    }
    let mut status = 0;
    while unsafe { libc::waitpid(pid, &mut status, 0) } < 0
        && unsafe { *libc::__errno() } == libc::EINTR
    {}
    if libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0 {
        JNI_TRUE
    } else {
        log::error!("writeBlock failed: status={status}");
        JNI_FALSE
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_findVendorBoot(
    mut env: EnvUnowned,
    _this: JObject,
    slot: JString,
    allow_slotless: jboolean,
) -> jstring {
    let slot = get_utf(&mut env, &slot).unwrap_or_default();
    let mut candidates = Vec::new();
    for base in ["/dev/block/by-name", "/dev/block/bootdevice/by-name"] {
        candidates.push(format!("{base}/vendor_boot{slot}"));
        if allow_slotless {
            candidates.push(format!("{base}/vendor_boot"));
        }
    }
    let candidates: Vec<CString> = candidates
        .into_iter()
        .filter_map(|s| CString::new(s).ok())
        .collect();
    let mut pipe = [0; 2];
    if unsafe { libc::pipe(pipe.as_mut_ptr()) } != 0 {
        return ptr::null_mut();
    }
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        unsafe {
            libc::close(pipe[0]);
            libc::close(pipe[1]);
        }
        return ptr::null_mut();
    }
    if pid == 0 {
        unsafe {
            libc::close(pipe[0]);
            if libc::setresuid(0, 0, 0) != 0 || libc::getuid() != 0 {
                libc::_exit(1);
            }
            for candidate in &candidates {
                if libc::access(candidate.as_ptr(), libc::F_OK) != 0 {
                    continue;
                }
                let bytes = candidate.as_bytes_with_nul();
                let mut off = 0;
                while off < bytes.len() {
                    let w = libc::write(pipe[1], bytes.as_ptr().add(off).cast(), bytes.len() - off);
                    if w > 0 {
                        off += w as usize;
                    } else if w < 0 && *libc::__errno() == libc::EINTR {
                        continue;
                    } else {
                        libc::_exit(2);
                    }
                }
                libc::close(pipe[1]);
                libc::_exit(0);
            }
            libc::_exit(3);
        }
    }
    unsafe {
        libc::close(pipe[1]);
    }
    let mut data = Vec::new();
    let mut buffer = [0u8; 256];
    loop {
        let n = unsafe { libc::read(pipe[0], buffer.as_mut_ptr().cast(), buffer.len()) };
        if n > 0 {
            data.extend_from_slice(&buffer[..n as usize]);
        } else if n <= 0 {
            break;
        }
    }
    unsafe {
        libc::close(pipe[0]);
    }
    let mut status = 0;
    while unsafe { libc::waitpid(pid, &mut status, 0) } < 0
        && unsafe { *libc::__errno() } == libc::EINTR
    {}
    if data.is_empty() || !libc::WIFEXITED(status) || libc::WEXITSTATUS(status) != 0 {
        log::warn!("findVendorBoot(slot={slot}) not found");
        return ptr::null_mut();
    }
    let end = data.iter().position(|b| *b == 0).unwrap_or(data.len());
    put_string(&mut env, &String::from_utf8_lossy(&data[..end]))
}

fn parse_gki(release: &str) -> Option<(bool, String)> {
    let mut parts = release.split('.');
    let major: i32 = parts.next()?.parse().ok()?;
    let minor_part = parts.next()?;
    let minor: i32 = minor_part
        .split(|c: char| !c.is_ascii_digit())
        .next()?
        .parse()
        .ok()?;
    let mut gki = false;
    if let Some(tag) = release.find("-android") {
        let rest = &release[tag + "-android".len()..];
        let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
        if !digits.is_empty() {
            let after = &rest[digits.len()..];
            if let Some(after) = after.strip_prefix('-')
                && after.chars().next().is_some_and(|c| c.is_ascii_digit())
            {
                gki = true;
            }
        }
    }
    Some((gki, format!("{major}.{minor:02}")))
}

fn uname_release() -> Option<String> {
    let mut uts: libc::utsname = unsafe { std::mem::zeroed() };
    if unsafe { libc::uname(&mut uts) } != 0 {
        return None;
    }
    let release = unsafe { CStr::from_ptr(uts.release.as_ptr()) };
    Some(release.to_string_lossy().into_owned())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_isGki(
    _env: EnvUnowned,
    _this: JObject,
) -> jboolean {
    uname_release()
        .and_then(|r| parse_gki(&r))
        .map(|(gki, _)| if gki { JNI_TRUE } else { JNI_FALSE })
        .unwrap_or(JNI_FALSE)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_kernelVersion(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    match uname_release().and_then(|r| parse_gki(&r)) {
        Some((_, version)) => put_string(&mut env, &version),
        None => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_kernelRelease(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    match uname_release() {
        Some(release) => put_string(&mut env, &release),
        None => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_moduleVersion(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    let Some(fd) = ensure_ctl_fd() else {
        return ptr::null_mut();
    };
    let mut data = [0u8; 64];
    if ioc_call(fd, IOC_GET_VERSION, &mut data) < 0 {
        return ptr::null_mut();
    }
    let end = data.iter().position(|b| *b == 0).unwrap_or(data.len());
    put_string(&mut env, &String::from_utf8_lossy(&data[..end]))
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_featureList(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    let Some(fd) = ensure_ctl_fd() else {
        return ptr::null_mut();
    };
    let mut data = [0u8; 1024];
    if ioc_call(fd, IOC_FEATURE_LIST, &mut data) < 0 {
        return ptr::null_mut();
    }
    let end = data.iter().position(|b| *b == 0).unwrap_or(data.len());
    put_string(&mut env, &String::from_utf8_lossy(&data[..end]))
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_featureGet(
    _env: EnvUnowned,
    _this: JObject,
    id: jint,
) -> jlong {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    let mut data = [0u8; FMAC_DATA_FEATURE];
    data[..4].copy_from_slice(&(id as u32).to_ne_bytes());
    if ioc_call(fd, IOC_FEATURE_GET, &mut data) < 0 {
        return -1;
    }
    u64::from_ne_bytes(
        data[FMAC_OFF_FEATURE_VALUE..FMAC_OFF_FEATURE_VALUE + 8]
            .try_into()
            .unwrap(),
    ) as jlong
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_featureSet(
    _env: EnvUnowned,
    _this: JObject,
    id: jint,
    value: jlong,
) -> jint {
    let Some(fd) = ensure_ctl_fd() else { return -1 };
    let mut data = [0u8; FMAC_DATA_FEATURE];
    data[..4].copy_from_slice(&(id as u32).to_ne_bytes());
    data[FMAC_OFF_FEATURE_VALUE..FMAC_OFF_FEATURE_VALUE + 8]
        .copy_from_slice(&(value as u64).to_ne_bytes());
    if ioc_call(fd, IOC_FEATURE_SET, &mut data) < 0 {
        -1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_getSlotSuffix(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    unsafe extern "C" {
        fn __system_property_get(
            name: *const libc::c_char,
            value: *mut libc::c_char,
        ) -> libc::c_int;
    }
    let mut value = [0 as libc::c_char; 92];
    let key = CString::new("ro.boot.slot_suffix").unwrap();
    let len = unsafe { __system_property_get(key.as_ptr(), value.as_mut_ptr()) };
    if len > 0 && value[0] != 0 {
        let text = unsafe { CStr::from_ptr(value.as_ptr()) };
        return put_string(&mut env, &text.to_string_lossy());
    }
    let key = CString::new("ro.boot.slot").unwrap();
    let len = unsafe { __system_property_get(key.as_ptr(), value.as_mut_ptr()) };
    if len > 0 && value[0] != 0 {
        let text = unsafe { CStr::from_ptr(value.as_ptr()) };
        return put_string(&mut env, &format!("_{}", text.to_string_lossy()));
    }
    ptr::null_mut()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_helloLog(_env: EnvUnowned, _this: JObject) {
    log::info!("ncore jni (Rust) loaded");
}

/// # Safety
///
/// `vm` must be a valid, non-null `JavaVM` pointer supplied by the JVM.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn JNI_OnLoad(
    _vm: *mut jni::sys::JavaVM,
    _reserved: *mut libc::c_void,
) -> jint {
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Info)
            .with_tag("ncore"),
    );
    log::info!("JNI loaded (conventional binding)");
    JNI_VERSION_1_6
}

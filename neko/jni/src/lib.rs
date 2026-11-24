// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg(target_os = "android")]
//! Rust replacement for the manager's former C++ JNI layer.
#![allow(dead_code)]

mod patch;

use std::ffi::{CStr, CString};
use std::os::fd::RawFd;
use std::ptr;
use std::sync::{Mutex, Once};

use jni::EnvUnowned;
use jni::errors::ThrowRuntimeExAndDefault;
use jni::objects::JByteArray;
use jni::objects::{JObject, JString};
use jni::sys::{JNI_FALSE, JNI_TRUE, JNI_VERSION_1_6, jboolean, jbyteArray, jint, jlong, jstring};
use kzcall::*;
use proto::FeatureEntry;
use proto::ProfileEntry;
use store;

const READER_MAX_PATH: usize = 4096;
const READER_CHUNK: usize = 64 * 1024;

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

/// jni 0.22 aliases `jboolean` to `bool` on Android and to `u8` elsewhere, so
/// compare against the type's default instead of the integer literal 0.
fn jboolean_is_true(value: jboolean) -> bool {
    value != jboolean::default()
}

const PR_GET_SHM_FD: libc::c_int = 201;
const PR_GET_CTL_FD: libc::c_int = 203;
const FMAC_SHM_NAME: &str = "[fmac_shm]";
const FMAC_CTL_NAME: &str = "[fmac_ctl]";

/// prctl() only attaches the anon fd; the tracepoint path does not rewrite the
/// syscall return value, so success is detected from `/proc/self/fd`.
fn anon_fd_present(name: &str) -> bool {
    let Ok(entries) = std::fs::read_dir("/proc/self/fd") else {
        return false;
    };
    entries.flatten().any(|entry| {
        std::fs::read_link(entry.path())
            .is_ok_and(|target| target.to_string_lossy().ends_with(name))
    })
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_ctl(
    _env: EnvUnowned,
    _this: JObject,
    value: jint,
) -> jint {
    let (cmd, fd_name) = match value {
        1 => (PR_GET_SHM_FD, FMAC_SHM_NAME),
        3 => (PR_GET_CTL_FD, FMAC_CTL_NAME),
        _ => return -1,
    };

    unsafe { libc::prctl(cmd, 0, 0, 0, 0) };
    if anon_fd_present(fd_name) {
        0
    } else {
        log::debug!("ctl({value}) did not attach {fd_name}");
        -1
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_setProfileProto(
    mut env: EnvUnowned,
    _this: JObject,
    data: JByteArray,
) -> jint {
    env.with_env(|env| -> jni::errors::Result<jint> {
        let bytes = env.convert_byte_array(&data)?;

        let profile = match ProfileEntry::from_bytes(&bytes) {
            Ok(p) => p,
            Err(e) => {
                log::error!("decode ProfileEntry failed: {e}");
                return Ok(-1);
            }
        };

        if let Err(e) = set_profile(&profile) {
            log::error!("set_profile failed: {e:#}");
        }
        match store::profile::save(&profile) {
            Ok(()) => Ok(0),
            Err(e) => {
                log::error!("save_profile failed: {e:#}");
                Ok(-1)
            }
        }
    })
    .resolve::<ThrowRuntimeExAndDefault>()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_adduid(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    match add_uid(uid) {
        Ok(()) => 0,
        Err(e) => {
            log::error!("add_uid failed: {e:#}");
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_deluid(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    if uid < 0 {
        log::error!("del_uid: negative uid {uid}");
        return -1;
    }
    if let Err(e) = del_uid(uid) {
        log::error!("del_uid failed: {e:#}");
        return -1;
    }
    match store::profile::del(uid as u32) {
        Ok(()) => 0,
        Err(e) => {
            log::error!("del_uid failed: {e:#}");
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_hasuid(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    match has_uid(uid) {
        Some(true) => 1,
        Some(false) => 0,
        None => -1,
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_setCap(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
    caps: jlong,
) -> jint {
    match set_cap(uid, caps as u64) {
        Ok(()) => 0,
        Err(e) => {
            log::error!("set_cap failed: {e:#}");
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_getCap(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jlong {
    match get_cap(uid) {
        Some(caps) => caps as jlong,
        None => -1,
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_delCap(
    _env: EnvUnowned,
    _this: JObject,
    uid: jint,
) -> jint {
    match del_cap(uid) {
        Ok(()) => 0,
        Err(e) => {
            log::error!("del_cap failed: {e:#}");
            -1
        }
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
    let src = get_utf(&mut env, &src).unwrap_or_default();
    let tgt = get_utf(&mut env, &tgt).unwrap_or_default();
    let cls = get_utf(&mut env, &cls).unwrap_or_default();
    let perm = get_utf(&mut env, &perm).unwrap_or_default();
    match add_selinux_rule(&src, &tgt, &cls, &perm, effect, jboolean_is_true(invert)) {
        Ok(()) => 0,
        Err(e) => {
            log::error!("add_selinux_rule failed: {e:#}");
            -1
        }
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
    match list_profiles() {
        Some(text) => put_string(&mut env, &text),
        None => {
            log::error!("listProfiles failed");
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_moduleVersion(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    match module_version() {
        Some(text) => put_string(&mut env, &text),
        None => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_featureList(
    mut env: EnvUnowned,
    _this: JObject,
) -> jstring {
    match feature_list() {
        Some(text) => put_string(&mut env, &text),
        None => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_featureGet(
    _env: EnvUnowned,
    _this: JObject,
    id: jint,
) -> jlong {
    match feature_get(id) {
        Some(value) => value as jlong,
        None => -1,
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_featureSet(
    mut env: EnvUnowned,
    _this: JObject,
    data: JByteArray,
) -> jint {
    env.with_env(|env| -> jni::errors::Result<jint> {
        let bytes = env.convert_byte_array(&data)?;

        let feat = match FeatureEntry::from_bytes(&bytes) {
            Ok(p) => p,
            Err(e) => {
                log::error!("decode FeatureEntry failed: {e}");
                return Ok(-1);
            }
        };

        if let Err(e) = feature_set(&feat) {
            log::error!("set_feature failed: {e:#}");
            return Ok(-1);
        }
        match store::feature::save_feature(&feat) {
            Ok(()) => Ok(0),
            Err(e) => {
                log::error!("set_feature failed: {e:#}");
                Ok(-1)
            }
        }
    })
    .resolve::<ThrowRuntimeExAndDefault>()
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_patchVendorBoot(
    mut env: EnvUnowned,
    _this: JObject,
    image: JString,
    module: JString,
    boot: JString,
    verify_symbols: jboolean,
    output: JString,
) -> jstring {
    let (Some(image), Some(module), Some(output)) = (
        get_utf(&mut env, &image),
        get_utf(&mut env, &module),
        get_utf(&mut env, &output),
    ) else {
        return put_string(&mut env, "patchVendorBoot: invalid path argument");
    };
    let boot = get_utf(&mut env, &boot);

    match patch::patch_vendor_boot(
        &image,
        &module,
        boot.as_deref(),
        jboolean_is_true(verify_symbols),
        &output,
    ) {
        Ok(()) => ptr::null_mut(),
        Err(e) => {
            let message = format!("{e:#}");
            log::error!("patchVendorBoot failed: {message}");
            put_string(&mut env, &message)
        }
    }
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

fn write_block_impl(src: &str, dst: &str) -> bool {
    let (Ok(src), Ok(dst)) = (CString::new(src), CString::new(dst)) else {
        return false;
    };
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        return false;
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
        true
    } else {
        log::error!("writeBlock failed: status={status}");
        false
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
    if write_block_impl(&src, &dst) {
        JNI_TRUE
    } else {
        JNI_FALSE
    }
}

/// Find a block-device path through `/dev/block/by-name` (or bootdevice) as
/// root. The lookup runs in a forked child so the caller does not need to hold
/// root itself.
fn find_partition(partition: &str, slot: &str, allow_slotless: bool) -> Option<String> {
    let mut candidates = Vec::new();
    for base in ["/dev/block/by-name", "/dev/block/bootdevice/by-name"] {
        candidates.push(format!("{base}/{partition}{slot}"));
        if allow_slotless {
            candidates.push(format!("{base}/{partition}"));
        }
    }
    let candidates: Vec<CString> = candidates
        .into_iter()
        .filter_map(|s| CString::new(s).ok())
        .collect();
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
        log::warn!("{partition}(slot={slot}) not found");
        return None;
    }
    let end = data.iter().position(|b| *b == 0).unwrap_or(data.len());
    Some(String::from_utf8_lossy(&data[..end]).into_owned())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_findVendorBoot(
    mut env: EnvUnowned,
    _this: JObject,
    slot: JString,
    allow_slotless: jboolean,
) -> jstring {
    let slot = get_utf(&mut env, &slot).unwrap_or_default();
    match find_partition("vendor_boot", &slot, jboolean_is_true(allow_slotless)) {
        Some(path) => put_string(&mut env, &path),
        None => ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_me_nekosu_aqnya_core_Ncore_findBoot(
    mut env: EnvUnowned,
    _this: JObject,
    slot: JString,
    allow_slotless: jboolean,
) -> jstring {
    let slot = get_utf(&mut env, &slot).unwrap_or_default();
    match find_partition("boot", &slot, jboolean_is_true(allow_slotless)) {
        Some(path) => put_string(&mut env, &path),
        None => ptr::null_mut(),
    }
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

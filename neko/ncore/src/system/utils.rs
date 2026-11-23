// SPDX-License-Identifier: GPL-3.0-or-later
use crate::defs::KSU_TEMP_BACKUP_DIR_NAME;
use crate::{assets, defs, selinux::restorecon};
use anyhow::{Context, Error, Result, bail};
use rustix::process::setpgid;
use rustix::{
    process,
    thread::{LinkNameSpaceType, move_into_link_name_space},
};
#[allow(unused_imports)]
use std::fs::{Permissions, set_permissions};
use std::os::unix::prelude::PermissionsExt;
use std::path::PathBuf;
use std::{
    ffi::{CString, c_char},
    fs::{File, OpenOptions, create_dir_all, remove_file, write},
    io::{
        BufRead, BufReader,
        ErrorKind::{AlreadyExists, NotFound},
        Write,
    },
    path::Path,
};

unsafe extern "C" {
    // Exported by bionic since the first Android release and present in every
    // NDK stub, unlike __system_property_read_callback (API 26+).
    fn __system_property_get(name: *const c_char, value: *mut c_char) -> std::os::raw::c_int;
}

#[macro_export]
macro_rules! debug_select {
    ($debug:expr, $release:expr) => {{
        #[cfg(debug_assertions)]
        {
            $debug
        }
        #[cfg(not(debug_assertions))]
        {
            $release
        }
    }};
}

pub fn ensure_clean_dir(dir: impl AsRef<Path>) -> Result<()> {
    let path = dir.as_ref();
    log::debug!("ensure_clean_dir: {}", path.display());
    if path.exists() {
        log::debug!("ensure_clean_dir: {} exists, remove it", path.display());
        std::fs::remove_dir_all(path)?;
    }
    Ok(std::fs::create_dir_all(path)?)
}

pub fn ensure_file_exists<T: AsRef<Path>>(file: T) -> Result<()> {
    match File::options().write(true).create_new(true).open(&file) {
        std::result::Result::Ok(_) => Ok(()),
        Err(err) => {
            if err.kind() == AlreadyExists && file.as_ref().is_file() {
                Ok(())
            } else {
                Err(Error::from(err))
                    .with_context(|| format!("{} is not a regular file", file.as_ref().display()))
            }
        }
    }
}

pub fn ensure_dir_exists<T: AsRef<Path>>(dir: T) -> Result<()> {
    let result = create_dir_all(&dir);
    if dir.as_ref().is_dir() && result.is_ok() {
        Ok(())
    } else {
        bail!("{} is not a regular directory", dir.as_ref().display())
    }
}

pub fn ensure_binary<T: AsRef<Path>>(
    path: T,
    contents: &[u8],
    ignore_if_exist: bool,
) -> Result<()> {
    if ignore_if_exist && path.as_ref().exists() {
        return Ok(());
    }

    ensure_dir_exists(path.as_ref().parent().ok_or_else(|| {
        anyhow::anyhow!(
            "{} does not have parent directory",
            path.as_ref().to_string_lossy()
        )
    })?)?;

    if let Err(e) = remove_file(path.as_ref())
        && e.kind() != NotFound
    {
        return Err(Error::from(e))
            .with_context(|| format!("failed to unlink {}", path.as_ref().display()));
    }

    write(&path, contents)?;
    set_permissions(&path, Permissions::from_mode(0o755))?;
    Ok(())
}

pub fn getprop(name: &str) -> Option<String> {
    let name = CString::new(name).ok()?;
    let mut value = [0 as c_char; 92];
    let len = unsafe { __system_property_get(name.as_ptr(), value.as_mut_ptr()) };
    if len <= 0 {
        return None;
    }
    let bytes = unsafe { std::slice::from_raw_parts(value.as_ptr().cast::<u8>(), len as usize) };
    Some(String::from_utf8_lossy(bytes).into_owned())
}

pub fn is_safe_mode() -> bool {
    let safemode = getprop("persist.sys.safemode")
        .as_ref()
        .is_some_and(|prop| prop == "1")
        || getprop("ro.sys.safemode")
            .as_ref()
            .is_some_and(|prop| prop == "1");
    log::info!("safemode: {safemode}");
    if safemode {
        return true;
    }
    match kzcall::check_kernel_safemode() {
        Ok(safemode) => {
            log::info!("kernel_safemode: {safemode}");
            safemode
        }
        Err(e) => {
            log::warn!("kernel safemode query failed: {e}");
            false
        }
    }
}

pub fn get_zip_uncompressed_size(zip_path: &str) -> Result<u64> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(zip_path)?)?;
    let mut total: u64 = 0;
    for i in 0..zip.len() {
        total += zip.by_index(i)?.size();
    }
    Ok(total)
}

pub fn switch_mnt_ns(pid: i32) -> Result<()> {
    use rustix::{
        fd::AsFd,
        fs::{Mode, OFlags, open},
    };
    let path = format!("/proc/{pid}/ns/mnt");
    let fd = open(path, OFlags::RDONLY, Mode::from_raw_mode(0))?;
    let current_dir = std::env::current_dir();
    move_into_link_name_space(fd.as_fd(), Some(LinkNameSpaceType::Mount))?;
    if let std::result::Result::Ok(current_dir) = current_dir {
        let _ = std::env::set_current_dir(current_dir);
    }
    Ok(())
}

fn switch_cgroup(grp: &str, pid: u32) {
    let path = Path::new(grp).join("cgroup.procs");
    if !path.exists() {
        return;
    }

    let fp = OpenOptions::new().append(true).open(path);
    if let std::result::Result::Ok(mut fp) = fp {
        let _ = write!(fp, "{pid}");
    }
}

pub fn switch_cgroups() {
    let pid = std::process::id();
    switch_cgroup("/acct", pid);
    switch_cgroup("/dev/cg2_bpf", pid);
    switch_cgroup("/sys/fs/cgroup", pid);

    if getprop("ro.config.per_app_memcg")
        .as_ref()
        .is_none_or(|prop| prop != "false")
    {
        switch_cgroup("/dev/memcg/apps", pid);
    }
}

pub fn umask(mask: u32) {
    process::umask(rustix::fs::Mode::from_raw_mode(mask));
}

pub fn has_magisk() -> bool {
    which::which("magisk").is_ok()
}

fn link_ncore_to_bin() -> Result<()> {
    let ksu_bin = PathBuf::from(defs::DAEMON_PATH);
    let ksu_bin_link = PathBuf::from(defs::DAEMON_LINK_PATH);
    if ksu_bin.exists() && !ksu_bin_link.exists() {
        std::os::unix::fs::symlink(&ksu_bin, &ksu_bin_link)?;
    }
    Ok(())
}

pub fn install() -> Result<()> {
    ensure_dir_exists(defs::ADB_DIR)?;
    let _ = std::fs::remove_file(defs::DAEMON_PATH);
    std::fs::copy(
        // We should use /proc/self/exe, DO NOT resolve the real path
        // So that if someone execute /data/adb/ncore install, ncore won't be removed unexpectedly
        "/proc/self/exe",
        defs::DAEMON_PATH,
    )?;
    restorecon::lsetfilecon(defs::DAEMON_PATH, restorecon::KSU_CON)?;
    // install binary assets
    assets::ensure_binaries(false).with_context(|| "Failed to extract assets")?;

    link_ncore_to_bin()?;

    Ok(())
}

pub fn detach_process_group(use_init_pgrp: bool) {
    if use_init_pgrp {
        if let Err(e) = kzcall::set_init_pgrp() {
            log::error!("failed to switch to init group: {e:?}");
        } else {
            return;
        }
    }
    if let Err(e2) = setpgid(None, None) {
        log::error!("failed to set process group: {e2:?}");
    }
}

#[derive(Debug)]
#[allow(dead_code)]
pub struct ModuleInfo {
    pub name: String,
    pub size: u64,
    pub refcount: u32,
    pub dependencies: Vec<String>,
    pub state: String,
    pub address: String,
}

impl ModuleInfo {
    fn parse(line: &str) -> Option<Self> {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 6 {
            return None;
        }

        let name = fields[0].to_string();
        let size = fields[1].parse::<u64>().unwrap_or(0);
        let refcount = fields[2].parse::<u32>().unwrap_or(0);

        let dependencies = if fields[3] == "-" {
            Vec::new()
        } else {
            fields[3].split(',').map(String::from).collect()
        };

        let state = fields[4].to_string();
        let address = fields[5].to_string();

        Some(ModuleInfo {
            name,
            size,
            refcount,
            dependencies,
            state,
            address,
        })
    }
}

fn find_module(target: &str) -> anyhow::Result<Option<ModuleInfo>> {
    let file = File::open("/proc/modules")?;
    let reader = BufReader::new(file);

    for line in reader.lines() {
        let line = line?;
        if line.is_empty() {
            continue;
        }
        if let Some(info) = ModuleInfo::parse(&line) {
            if info.name == target {
                return Ok(Some(info));
            }
        }
    }
    Ok(None)
}

pub fn runtime_mode() -> &'static str {
    match find_module("nksu") {
        std::result::Result::Ok(Some(_)) => "lkm",
        std::result::Result::Ok(None) => "built-in",
        std::result::Result::Err(_) => "unknown",
    }
}

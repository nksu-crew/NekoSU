use crate::defs::*;
use crate::fd;
use anyhow::{Result, anyhow, bail};
use std::io;
use std::os::fd::RawFd;

/// Build the `struct fmac_ioc` header followed by the flag payload.
fn make_buffer(flag: u32, data: &[u8]) -> Result<Vec<u8>> {
    let len = u32::try_from(data.len())
        .map_err(|_| anyhow!("NekoSU ioctl {flag} payload is too large: {}", data.len()))?;
    let mut buffer = Vec::with_capacity(8 + data.len());
    buffer.extend_from_slice(&flag.to_ne_bytes());
    buffer.extend_from_slice(&len.to_ne_bytes());
    buffer.extend_from_slice(data);
    Ok(buffer)
}

fn invoke(fd: RawFd, flag: u32, buffer: &mut [u8]) -> Result<i32> {
    let ret = unsafe { libc::ioctl(fd, IOC_CMD, buffer.as_mut_ptr()) };
    if ret < 0 {
        bail!("NekoSU ioctl {flag} failed: {}", io::Error::last_os_error());
    }
    Ok(ret)
}

/// Run an ioctl whose payload is both input and output.
pub fn call(flag: u32, data: &mut [u8]) -> Result<i32> {
    let fd = fd::driver_fd()?;
    let mut buffer = make_buffer(flag, data)?;
    let ret = invoke(fd, flag, &mut buffer)?;
    data.copy_from_slice(&buffer[8..]);
    Ok(ret)
}

/// Run an ioctl with an input-only payload, avoiding a copy-back buffer.
pub fn call_in(flag: u32, data: &[u8]) -> Result<i32> {
    let fd = fd::driver_fd()?;
    let mut buffer = make_buffer(flag, data)?;
    invoke(fd, flag, &mut buffer)
}

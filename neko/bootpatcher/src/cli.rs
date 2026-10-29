use anyhow::{Context, Result, bail};
use std::env;

pub struct Args {
    pub image: String,
    pub module: String,
    pub boot: Option<String>,
    pub verify_symbols: bool,
}

pub const USAGE: &str = "\
usage: bootpatcher -p <vendor_boot.img> <nksu.ko> -b <boot.img>
       bootpatcher -p <vendor_boot.img> <nksu.ko> --no-symbol-check

  -p, --patch              vendor_boot image and nksu.ko to install
  -b, --boot               boot image containing the target kernel
      --no-symbol-check    skip the kernel symbol verification
  -h, --help               show this help";

pub fn parse_args() -> Result<Option<Args>> {
    let mut args = env::args().skip(1);
    let mut image = None;
    let mut module = None;
    let mut boot = None;
    let mut verify_symbols = true;

    while let Some(argument) = args.next() {
        match argument.as_str() {
            "-p" | "--patch" => {
                if image.is_some() || module.is_some() {
                    bail!("-p/--patch was given more than once");
                }
                image = Some(
                    args.next()
                        .context("-p/--patch needs a vendor_boot image")?,
                );
                module = Some(args.next().context("-p/--patch needs an nksu.ko")?);
            }
            "-b" | "--boot" => {
                if boot.is_some() {
                    bail!("-b/--boot was given more than once");
                }
                boot = Some(args.next().context("-b/--boot needs a boot image")?);
            }
            "--no-symbol-check" => verify_symbols = false,
            "-h" | "--help" => return Ok(None),
            other => bail!("unexpected argument '{other}'\n{USAGE}"),
        }
    }

    Ok(Some(Args {
        image: image.context("missing -p <vendor_boot.img> <nksu.ko>")?,
        module: module.context("missing -p <vendor_boot.img> <nksu.ko>")?,
        boot,
        verify_symbols,
    }))
}

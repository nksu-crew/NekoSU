// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * neko-boot -- patch an LKM into a v3/v4 vendor_boot image.
 *
 * This replaces the boot-patching half of ncore.  The manager calls it as
 *
 *     neko-boot -p <vendor_boot.img> <nksu.ko>
 *
 * and it writes the patched image to `<vendor_boot.img>.new`, keeping the
 * ramdisk's original compression and updating the v4 vendor ramdisk table.
 */

use std::env;
use std::fs;
use std::io::Cursor;
use std::path::Path;
use std::process::ExitCode;

use android_bootimg::cpio::{Cpio, CpioEntry};
use android_bootimg::parser::{BootImage, BootImageVersion};
use android_bootimg::patcher::BootImagePatchOption;
use anyhow::{Context, Result, bail};

const MODULE_PATH: &str = "lib/modules/nksu.ko";

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("neko-boot: {error:#}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let mut args = env::args().skip(1);
    let (image, module) = match (args.next().as_deref(), args.next(), args.next()) {
        (Some("-p" | "--patch"), Some(image), Some(module)) => (image, module),
        _ => bail!("usage: neko-boot -p <vendor_boot.img> <nksu.ko>"),
    };

    let image_path = Path::new(&image);
    let module_data = fs::read(&module).with_context(|| format!("read {module}"))?;
    let image_data = fs::read(image_path).with_context(|| format!("read {image}"))?;
    let boot = BootImage::parse(&image_data).with_context(|| format!("parse {image}"))?;

    match boot.get_header().get_version() {
        BootImageVersion::Vendor(3 | 4) => {}
        version => bail!("only vendor_boot v3/v4 images are supported (got {version:?})"),
    }

    let ramdisk = boot
        .get_blocks()
        .get_ramdisk()
        .context("vendor_boot has no ramdisk")?;

    // v4 splits the vendor ramdisk into table entries; v3 has one blob.  Find
    // the entry that already carries lib/modules, falling back to the first
    // entry that parses as a cpio archive.
    let mut candidates: Vec<(Option<usize>, Cpio, bool)> = Vec::new();
    if ramdisk.is_vendor_ramdisk() {
        for (index, entry) in ramdisk.iter_vendor_ramdisk().enumerate() {
            let mut raw = Vec::new();
            if entry.dump(&mut raw, false).is_err() {
                continue;
            }
            let Ok(cpio) = Cpio::load_from_data(&raw) else {
                continue;
            };
            let has_modules = cpio
                .entries()
                .keys()
                .any(|name| name == "lib/modules" || name.starts_with("lib/modules/"));
            candidates.push((Some(index), cpio, has_modules));
        }
    } else {
        let mut raw = Vec::new();
        ramdisk.dump(&mut raw, false)?;
        candidates.push((None, Cpio::load_from_data(&raw)?, true));
    }

    let chosen = candidates
        .iter()
        .position(|(_, _, has_modules)| *has_modules)
        .or_else(|| (!candidates.is_empty()).then_some(0))
        .context("no cpio vendor ramdisk found in the image")?;
    let (vendor_index, mut cpio, _) = candidates.swap_remove(chosen);

    if !cpio.entries().contains_key("lib/modules") {
        if !cpio.entries().contains_key("lib") {
            cpio.add("lib", CpioEntry::dir(0o755))?;
        }
        cpio.add("lib/modules", CpioEntry::dir(0o755))?;
    }
    cpio.add(
        MODULE_PATH,
        CpioEntry::regular(0o644, Box::new(module_data)),
    )?;
    ensure_line(
        &mut cpio,
        "lib/modules/modules.load",
        &["nksu.ko"],
        "nksu.ko\n",
    )?;
    ensure_line(
        &mut cpio,
        "lib/modules/modules.dep",
        &["/lib/modules/nksu.ko:", "lib/modules/nksu.ko:"],
        "/lib/modules/nksu.ko:\n",
    )?;

    let mut new_cpio = Vec::new();
    cpio.dump(&mut new_cpio)?;

    let mut patcher = BootImagePatchOption::new(&boot);
    if let Some(index) = vendor_index {
        patcher.replace_vendor_ramdisk(index, Box::new(Cursor::new(new_cpio)), false);
    } else {
        patcher.replace_ramdisk(Box::new(Cursor::new(new_cpio)), false);
    }

    let mut output = Cursor::new(Vec::with_capacity(boot.get_size()));
    patcher.patch(&mut output)?;

    let output_path = format!("{}.new", image_path.display());
    fs::write(&output_path, output.into_inner()).with_context(|| format!("write {output_path}"))?;
    Ok(())
}

fn ensure_line(cpio: &mut Cpio, name: &str, needles: &[&str], line: &str) -> Result<()> {
    let existing = cpio.entries().get(name).and_then(|entry| entry.data());
    let mut text = existing
        .map(|data| String::from_utf8_lossy(data).into_owned())
        .unwrap_or_default();

    if needles.iter().any(|needle| has_line(&text, needle)) {
        return Ok(());
    }

    if !text.is_empty() && !text.ends_with('\n') {
        text.push('\n');
    }
    text.push_str(line);

    cpio.add(name, CpioEntry::regular(0o644, Box::new(text.into_bytes())))?;
    Ok(())
}

fn has_line(text: &str, needle: &str) -> bool {
    text.lines()
        .any(|line| line.trim_start().starts_with(needle))
}

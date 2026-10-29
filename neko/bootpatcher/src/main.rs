// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * bootpatcher -- patch an LKM into a v3/v4 vendor_boot image.
 *
 * This replaces the boot-patching half of ncore.  The manager calls it as
 *
 *     bootpatcher -p <vendor_boot.img> <nksu.ko> -b <boot.img>
 *
 * and it writes the patched image to `<vendor_boot.img>.new`, keeping the
 * ramdisk's original compression and updating the v4 vendor ramdisk table.
 *
 * Before touching the vendor_boot image it reads the symbol table that nksu.ko
 * carries in .nksu_ksymtab and verifies that every required kallsyms-resolved
 * symbol exists in the kernel extracted from <boot.img>. A module that would
 * fail to load on the target kernel is therefore rejected before anything is
 * flashed; --no-symbol-check skips that step and is surfaced in the manager's
 * settings for recovery scenarios.
 */

mod cli;
mod kallsyms;
mod symbols;

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
            eprintln!("bootpatcher: {error:#}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let Some(args) = cli::parse_args()? else {
        eprintln!("{}", cli::USAGE);
        return Ok(());
    };

    eprintln!("bootpatcher: reading module {}", args.module);
    let module_data = fs::read(&args.module).with_context(|| format!("read {}", args.module))?;

    if args.verify_symbols {
        verify_kernel_symbols(&module_data, args.boot.as_deref())?;
    } else {
        eprintln!("bootpatcher: symbol check disabled, skipping kernel verification");
    }

    eprintln!("bootpatcher: reading image {}", args.image);
    let image_data = fs::read(&args.image).with_context(|| format!("read {}", args.image))?;
    eprintln!(
        "bootpatcher: parsing boot image ({} bytes)",
        image_data.len()
    );
    let boot = BootImage::parse(&image_data).with_context(|| format!("parse {}", args.image))?;

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
    eprintln!("bootpatcher: locating vendor ramdisk");
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
    let format = match vendor_index {
        Some(index) => ramdisk
            .get_vendor_ramdisk(index)
            .map(|entry| entry.get_compress_format()),
        None => Some(ramdisk.get_compress_format()),
    };
    eprintln!("bootpatcher: selected ramdisk index={vendor_index:?}, compression={format:?}");

    eprintln!("bootpatcher: injecting lib/modules/nksu.ko");
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

    eprintln!("bootpatcher: repacking ramdisk");
    let mut new_cpio = Vec::new();
    cpio.dump(&mut new_cpio)?;

    let mut patcher = BootImagePatchOption::new(&boot);
    if let Some(index) = vendor_index {
        patcher.replace_vendor_ramdisk(index, Box::new(Cursor::new(new_cpio)), false);
    } else {
        patcher.replace_ramdisk(Box::new(Cursor::new(new_cpio)), false);
    }

    eprintln!("bootpatcher: repacking boot image");
    let mut output = Cursor::new(Vec::with_capacity(boot.get_size()));
    patcher.patch(&mut output)?;

    let output_path = format!("{}.new", Path::new(&args.image).display());
    eprintln!("bootpatcher: writing {output_path}");
    fs::write(&output_path, output.into_inner()).with_context(|| format!("write {output_path}"))?;
    eprintln!("bootpatcher: done");
    Ok(())
}

fn verify_kernel_symbols(module: &[u8], boot_path: Option<&str>) -> Result<()> {
    let exports = symbols::read(module)?;
    if exports.is_empty() {
        bail!(
            "module exports no kernel symbols in {} (built without the symbol export macro?)",
            symbols::KSYMTAB_SECTION
        );
    }

    let boot_path = boot_path.context(
        "missing -b <boot.img>: kernel symbol verification needs the target boot image \
         (or pass --no-symbol-check)",
    )?;

    eprintln!(
        "bootpatcher: module needs {} required and {} optional kernel symbol(s)",
        exports.required.len(),
        exports.optional.len()
    );
    eprintln!("bootpatcher: reading boot image {boot_path}");
    let boot_data = fs::read(boot_path).with_context(|| format!("read {boot_path}"))?;

    let mut kernel_data = Vec::new();
    {
        let boot = BootImage::parse(&boot_data).with_context(|| format!("parse {boot_path}"))?;
        let kernel = boot
            .get_blocks()
            .get_kernel()
            .with_context(|| format!("{boot_path} has no kernel"))?;
        eprintln!(
            "bootpatcher: extracting kernel (compression={:?})",
            kernel.get_compress_format()
        );
        kernel
            .dump(&mut kernel_data, false)
            .with_context(|| format!("extract kernel from {boot_path}"))?;
    }
    drop(boot_data);

    eprintln!("bootpatcher: decoding kallsyms");
    let available =
        kallsyms::parse_names(&kernel_data).context("read kallsyms from the target kernel")?;
    drop(kernel_data);
    eprintln!(
        "bootpatcher: kernel provides {} kallsyms symbol(s)",
        available.len()
    );

    let missing_required: Vec<&str> = exports
        .required
        .iter()
        .map(String::as_str)
        .filter(|name| !available.contains(*name))
        .collect();
    let missing_optional: Vec<&str> = exports
        .optional
        .iter()
        .map(String::as_str)
        .filter(|name| !available.contains(*name))
        .collect();

    for name in &missing_optional {
        eprintln!("bootpatcher: warning: optional symbol '{name}' is not present");
    }

    if !missing_required.is_empty() {
        for name in &missing_required {
            eprintln!("bootpatcher: missing required symbol: {name}");
        }
        bail!(
            "target kernel lacks {} required symbol(s); refusing to patch",
            missing_required.len()
        );
    }

    eprintln!(
        "bootpatcher: all {} required kernel symbol(s) present",
        exports.required.len()
    );
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

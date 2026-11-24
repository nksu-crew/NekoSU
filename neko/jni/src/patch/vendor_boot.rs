// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Patch an LKM into a v3/v4 vendor_boot image.
 *
 * This used to be a separate `bootpatcher` executable invoked by the manager.
 * It now runs inside libjni, so the manager only needs a file path and gets the
 * patched image back on disk. The patcher keeps the ramdisk's original
 * compression, updates the v4 vendor ramdisk table and verifies the LKM's
 * .nksu_ksymtab symbols against the target kernel before writing anything.
 */

use std::fs;
use std::path::Path;

use anyhow::{Context, Result, bail};
use bootparse::cpio::{Cpio, CpioEntry};
use bootparse::parse::BootImage;
use bootparse::patcher::VendorBootPatcher;

use crate::patch::kallsyms;
use crate::patch::symbols;

const MODULE_PATH: &str = "lib/modules/nksu.ko";

pub fn patch_vendor_boot(
    image_path: &str,
    module_path: &str,
    boot_path: Option<&str>,
    verify_symbols: bool,
    output_path: &str,
) -> Result<()> {
    log::info!("patch: reading module {module_path}");
    let module_data = fs::read(module_path).with_context(|| format!("read {module_path}"))?;

    if verify_symbols {
        verify_kernel_symbols(&module_data, boot_path)?;
    } else {
        log::info!("patch: symbol check disabled, skipping kernel verification");
    }

    log::info!("patch: reading image {image_path}");
    let image_data = fs::read(image_path).with_context(|| format!("read {image_path}"))?;
    log::info!("patch: parsing boot image ({} bytes)", image_data.len());
    let image = BootImage::parse(&image_data).with_context(|| format!("parse {image_path}"))?;

    match image.vendor_boot_version() {
        Some(3 | 4) => {}
        version => bail!("only vendor_boot v3/v4 images are supported (got {version:?})"),
    }

    let ramdisks = image.ramdisks().context("vendor_boot has no ramdisk")?;

    // v4 splits the vendor ramdisk into table entries; v3 has one blob. Find
    // the entry that already carries lib/modules, falling back to the first
    // entry that parses as a cpio archive.
    log::info!("patch: locating vendor ramdisk");
    let mut candidates: Vec<(usize, Cpio, bool)> = Vec::new();
    for entry in ramdisks {
        let Ok(raw) = entry.decompress() else {
            continue;
        };
        let Ok(cpio) = Cpio::load_from_data(&raw) else {
            continue;
        };
        let has_modules = cpio
            .entries()
            .keys()
            .any(|name| name == "lib/modules" || name.starts_with("lib/modules/"));
        candidates.push((entry.index, cpio, has_modules));
    }

    let chosen = candidates
        .iter()
        .position(|(_, _, has_modules)| *has_modules)
        .or_else(|| (!candidates.is_empty()).then_some(0))
        .context("no cpio vendor ramdisk found in the image")?;
    let (vendor_index, mut cpio, _) = candidates.swap_remove(chosen);
    log::info!(
        "patch: selected ramdisk index={vendor_index}, compression={:?}",
        ramdisks[vendor_index].format()
    );

    log::info!("patch: injecting {MODULE_PATH}");
    if !cpio.exists("lib/modules") {
        if !cpio.exists("lib") {
            cpio.add("lib", CpioEntry::dir(0o755))?;
        }
        cpio.add("lib/modules", CpioEntry::dir(0o755))?;
    }
    cpio.add(MODULE_PATH, CpioEntry::regular(0o644, module_data))?;
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

    log::info!("patch: repacking ramdisk");
    let new_cpio = cpio.dump();

    log::info!("patch: repacking boot image");
    let mut patcher = VendorBootPatcher::new(&image)?;
    patcher.replace_vendor_ramdisk(vendor_index, new_cpio);
    let output = patcher.patch()?;

    let output_path = Path::new(output_path);
    if let Some(parent) = output_path.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).with_context(|| format!("create {}", parent.display()))?;
    }
    log::info!("patch: writing {}", output_path.display());
    fs::write(output_path, output).with_context(|| format!("write {}", output_path.display()))?;
    log::info!("patch: done");
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
        "missing boot image: kernel symbol verification needs the target boot image \
         (or disable symbol verification)",
    )?;

    log::info!(
        "patch: module needs {} required and {} optional kernel symbol(s)",
        exports.required.len(),
        exports.optional.len()
    );
    log::info!("patch: reading boot image {boot_path}");
    let boot_data = fs::read(boot_path).with_context(|| format!("read {boot_path}"))?;

    let kernel_data = {
        let boot = BootImage::parse(&boot_data).with_context(|| format!("parse {boot_path}"))?;
        let kernel = boot
            .kernel()
            .with_context(|| format!("{boot_path} has no kernel"))?;
        log::info!(
            "patch: extracting kernel (compression={:?})",
            kernel.format()
        );
        kernel
            .decompress()
            .with_context(|| format!("extract kernel from {boot_path}"))?
    };
    drop(boot_data);

    log::info!("patch: decoding kallsyms");
    let available =
        kallsyms::parse_names(&kernel_data).context("read kallsyms from the target kernel")?;
    drop(kernel_data);
    log::info!(
        "patch: kernel provides {} kallsyms symbol(s)",
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
        log::warn!("patch: optional symbol '{name}' is not present");
    }

    if !missing_required.is_empty() {
        for name in &missing_required {
            log::error!("patch: missing required symbol: {name}");
        }
        bail!(
            "target kernel lacks {} required symbol(s); refusing to patch",
            missing_required.len()
        );
    }

    log::info!(
        "patch: all {} required kernel symbol(s) present",
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

    cpio.add(name, CpioEntry::regular(0o644, text.into_bytes()))?;
    Ok(())
}

fn has_line(text: &str, needle: &str) -> bool {
    text.lines()
        .any(|line| line.trim_start().starts_with(needle))
}

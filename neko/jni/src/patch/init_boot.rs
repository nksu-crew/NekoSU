use std::fs;
use std::path::Path;

use anyhow::{Context, Result, bail};
use bootparse::cpio::{Cpio, CpioEntry};
use bootparse::parse::BootImage;
use bootparse::patcher::BootPatcher;

const MODULE_PATH: &str = "nksu.ko";
const INIT_PATH: &str = "init";
/// Tells ncore that this ramdisk's init wrapper contains the perinit runner.
const PREINIT_MARKER_PATH: &str = "nksu.preinit";
/// The bundled init wrapper loads the LKM and then execs this path, so the
/// original first-stage init must be preserved before `/init` is replaced.
const ORIGINAL_INIT_PATH: &str = "init.real";

pub fn patch_init_boot(
    image_path: &str,
    module_path: &str,
    init_path: &str,
    output_path: &str,
) -> Result<()> {
    log::info!("patch: reading module {module_path}");
    let module_data = fs::read(module_path).with_context(|| format!("read {module_path}"))?;

    let init_data = fs::read(init_path).with_context(|| format!("read {init_path}"))?;

    log::info!("patch: reading image {image_path}");
    let image_data = fs::read(image_path).with_context(|| format!("read {image_path}"))?;
    log::info!("patch: parsing boot image ({} bytes)", image_data.len());
    let image = BootImage::parse(&image_data).with_context(|| format!("parse {image_path}"))?;

    match image.boot_version() {
        Some(3 | 4) => {}
        version => bail!("only init_boot v3/v4 images are supported (got {version:?})"),
    }

    let raw = image.ramdisk().context("boot has no ramdisk")?;
    let draw = raw.decompress().context("boot has no ramdisk")?;
    let mut cpio = Cpio::load_from_data(&draw).context("failed to parse ramdisk")?;
    log::info!("patch: locating ramdisk");

    if !cpio.exists(ORIGINAL_INIT_PATH) {
        let original_init = cpio
            .get(INIT_PATH)
            .cloned()
            .context("init_boot ramdisk has no /init")?;
        log::info!("patch: preserving /{INIT_PATH} as /{ORIGINAL_INIT_PATH}");
        cpio.add(ORIGINAL_INIT_PATH, original_init)?;
    }

    log::info!("patch: injecting {MODULE_PATH}");
    cpio.add(INIT_PATH, CpioEntry::regular(0o755, init_data))?;

    cpio.add(MODULE_PATH, CpioEntry::regular(0o644, module_data))?;

    log::info!("patch: marking the perinit runner");
    cpio.add(PREINIT_MARKER_PATH, CpioEntry::regular(0o644, b"1\n".to_vec()))?;

    log::info!("patch: repacking ramdisk");
    let new_cpio = cpio.dump();

    log::info!("patch: repacking boot image");
    let mut patcher = BootPatcher::new(&image)?;
    patcher.replace_ramdisk(new_cpio);
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

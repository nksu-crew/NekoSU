// SPDX-License-Identifier: GPL-3.0-or-later
use anyhow::{Context, Result, bail};
use log::{debug, warn};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};

use crate::defs;

#[allow(clippy::unreadable_literal)]
const MODULE_CONFIG_MAGIC: u32 = 0x4b53554d; // "KSUM"
const MODULE_CONFIG_VERSION: u32 = 1;

// Validation limits
pub const MAX_CONFIG_KEY_LEN: usize = 256;
pub const MAX_CONFIG_VALUE_LEN: usize = 1024 * 1024; // 1MB
pub const MAX_CONFIG_COUNT: usize = 32;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConfigType {
    Persist,
    Temp,
}

impl ConfigType {
    const fn filename(self) -> &'static str {
        match self {
            Self::Persist => defs::PERSIST_CONFIG_NAME,
            Self::Temp => defs::TEMP_CONFIG_NAME,
        }
    }
}

/// Parse a boolean config value
/// Accepts "true", "1" (case-insensitive) as true, everything else as false
pub fn parse_bool_config(value: &str) -> bool {
    let trimmed = value.trim();
    trimmed.eq_ignore_ascii_case("true") || trimmed == "1"
}

/// Validate config key
/// Uses the same validation rules as module_id: ^[a-zA-Z][a-zA-Z0-9._-]+$
/// - Must start with a letter (a-zA-Z)
/// - Followed by one or more alphanumeric, dot, underscore, or hyphen characters
/// - Minimum length: 2 characters
fn get_config_dir(module_id: &str) -> PathBuf {
    Path::new(defs::MODULE_CONFIG_DIR).join(module_id)
}

/// Get the config file path for a module
fn get_config_path(module_id: &str, config_type: ConfigType) -> PathBuf {
    get_config_dir(module_id).join(config_type.filename())
}

/// Ensure the config directory exists
pub fn load_config(module_id: &str, config_type: ConfigType) -> Result<HashMap<String, String>> {
    crate::module::validate_module_id(module_id)?;

    let config_path = get_config_path(module_id, config_type);

    if !config_path.exists() {
        debug!("Config file not found: {}", config_path.display());
        return Ok(HashMap::new());
    }

    let mut file = File::open(&config_path)
        .with_context(|| format!("Failed to open config file: {}", config_path.display()))?;

    // Read magic
    let mut magic_buf = [0u8; 4];
    file.read_exact(&mut magic_buf)
        .with_context(|| "Failed to read magic")?;
    let magic = u32::from_le_bytes(magic_buf);

    if magic != MODULE_CONFIG_MAGIC {
        bail!("Invalid config magic: expected 0x{MODULE_CONFIG_MAGIC:08x}, got 0x{magic:08x}");
    }

    // Read version
    let mut version_buf = [0u8; 4];
    file.read_exact(&mut version_buf)
        .with_context(|| "Failed to read version")?;
    let version = u32::from_le_bytes(version_buf);

    if version != MODULE_CONFIG_VERSION {
        bail!("Unsupported config version: expected {MODULE_CONFIG_VERSION}, got {version}");
    }

    // Read count
    let mut count_buf = [0u8; 4];
    file.read_exact(&mut count_buf)
        .with_context(|| "Failed to read count")?;
    let count = u32::from_le_bytes(count_buf) as usize;

    if count > MAX_CONFIG_COUNT {
        bail!("Too many config entries: {count} (max: {MAX_CONFIG_COUNT})");
    }

    // Read entries
    let mut config = HashMap::new();
    for i in 0..count {
        // Read key length
        let mut key_len_buf = [0u8; 4];
        file.read_exact(&mut key_len_buf)
            .with_context(|| format!("Failed to read key length for entry {i}"))?;
        let key_len = u32::from_le_bytes(key_len_buf) as usize;

        if key_len > MAX_CONFIG_KEY_LEN {
            bail!("Config key too long for entry {i}: {key_len} bytes (max: {MAX_CONFIG_KEY_LEN})");
        }

        // Read key data
        let mut key_buf = vec![0u8; key_len];
        file.read_exact(&mut key_buf)
            .with_context(|| format!("Failed to read key data for entry {i}"))?;
        let key = String::from_utf8(key_buf)
            .with_context(|| format!("Invalid UTF-8 in key for entry {i}"))?;

        // Read value length
        let mut value_len_buf = [0u8; 4];
        file.read_exact(&mut value_len_buf)
            .with_context(|| format!("Failed to read value length for entry {i}"))?;
        let value_len = u32::from_le_bytes(value_len_buf) as usize;

        if value_len > MAX_CONFIG_VALUE_LEN {
            bail!(
                "Config value too long for entry {i}: {value_len} bytes (max: {MAX_CONFIG_VALUE_LEN})"
            );
        }

        // Read value data
        let mut value_buf = vec![0u8; value_len];
        file.read_exact(&mut value_buf)
            .with_context(|| format!("Failed to read value data for entry {i}"))?;
        let value = String::from_utf8(value_buf)
            .with_context(|| format!("Invalid UTF-8 in value for entry {i}"))?;

        config.insert(key, value);
    }

    debug!(
        "Loaded {} entries from {}",
        config.len(),
        config_path.display()
    );
    Ok(config)
}

pub fn merge_configs(module_id: &str) -> Result<HashMap<String, String>> {
    crate::module::validate_module_id(module_id)?;

    let mut merged = match load_config(module_id, ConfigType::Persist) {
        Ok(config) => config,
        Err(e) => {
            warn!("Failed to load persist config for module '{module_id}': {e}");
            HashMap::new()
        }
    };

    let temp = match load_config(module_id, ConfigType::Temp) {
        Ok(config) => config,
        Err(e) => {
            warn!("Failed to load temp config for module '{module_id}': {e}");
            HashMap::new()
        }
    };

    // Temp config overrides persist config
    for (key, value) in temp {
        merged.insert(key, value);
    }

    Ok(merged)
}

/// Get all module configs (for iteration)
/// Loads all configs in a single pass to minimize I/O overhead
pub fn get_all_module_configs() -> Result<HashMap<String, HashMap<String, String>>> {
    let config_root = Path::new(defs::MODULE_CONFIG_DIR);

    if !config_root.exists() {
        return Ok(HashMap::new());
    }

    let mut all_configs = HashMap::new();

    for entry in fs::read_dir(config_root)
        .with_context(|| format!("Failed to read config directory: {}", config_root.display()))?
    {
        let entry = entry?;
        let path = entry.path();

        if !path.is_dir() {
            continue;
        }

        if let Some(module_id) = path.file_name().and_then(|n| n.to_str()) {
            match merge_configs(module_id) {
                Ok(config) => {
                    if !config.is_empty() {
                        all_configs.insert(module_id.to_string(), config);
                    }
                }
                Err(e) => {
                    warn!("Failed to load config for module '{module_id}': {e}");
                    // Continue processing other modules
                }
            }
        }
    }

    Ok(all_configs)
}

/// Clear all temporary configs (called during post-fs-data)
pub fn clear_all_temp_configs() -> Result<()> {
    let config_root = Path::new(defs::MODULE_CONFIG_DIR);

    if !config_root.exists() {
        debug!("Config directory does not exist, nothing to clear");
        return Ok(());
    }

    let mut cleared_count = 0;

    for entry in fs::read_dir(config_root)
        .with_context(|| format!("Failed to read config directory: {}", config_root.display()))?
    {
        let entry = entry?;
        let path = entry.path();

        if !path.is_dir() {
            continue;
        }

        let temp_config = path.join(defs::TEMP_CONFIG_NAME);
        if temp_config.exists() {
            match fs::remove_file(&temp_config) {
                Ok(()) => {
                    debug!("Cleared temp config: {}", temp_config.display());
                    cleared_count += 1;
                }
                Err(e) => {
                    warn!("Failed to clear temp config {}: {e}", temp_config.display());
                }
            }
        }
    }

    if cleared_count > 0 {
        debug!("Cleared {cleared_count} temp config file(s)");
    }

    Ok(())
}

/// Clear all configs for a module (called during uninstall)
pub fn clear_module_configs(module_id: &str) -> Result<()> {
    crate::module::validate_module_id(module_id)?;

    let config_dir = get_config_dir(module_id);

    if config_dir.exists() {
        fs::remove_dir_all(&config_dir).with_context(|| {
            format!(
                "Failed to remove config directory: {}",
                config_dir.display()
            )
        })?;
        debug!("Cleared all configs for module: {module_id}");
    }

    Ok(())
}

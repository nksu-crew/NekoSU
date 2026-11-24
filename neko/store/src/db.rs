//! Unified profile/feature store.
//!
//! The file is laid out as:
//!
//! ```text
//! header | profile indexes | feature indexes | profile payloads | feature payloads
//! ```
//!
//! Each fixed-size [`RecordIndex`] stores the owning key and the absolute
//! `[offset, offset + len)` of its protobuf payload in the same file.
//!
//! This format is intentionally incompatible with the old
//! `allowed.profile` / `allowed.feature` files; they are never read.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};

use proto::{FeatureEntry, ProfileEntry};

use crate::root::ensure_root;

const STORE_PATH: &str = "/data/adb/nksu/profile";

const MAGIC: [u8; 8] = *b"NKSUPRF\0";
const VERSION: u32 = 1;
const HEADER_SIZE: usize = 24;
const INDEX_SIZE: usize = 16;

static SAVE_LOCK: Mutex<()> = Mutex::new(());

#[derive(Default)]
pub(crate) struct Store {
    pub(crate) profiles: Vec<ProfileEntry>,
    pub(crate) features: Vec<FeatureEntry>,
}

#[derive(Clone, Copy)]
struct Header {
    profile_count: u32,
    feature_count: u32,
}

#[derive(Clone, Copy, Debug)]
struct RecordIndex {
    key: u32,
    offset: u64,
    len: u32,
}

impl Header {
    fn encode(self) -> [u8; HEADER_SIZE] {
        let mut bytes = [0u8; HEADER_SIZE];
        bytes[..8].copy_from_slice(&MAGIC);
        bytes[8..12].copy_from_slice(&VERSION.to_le_bytes());
        bytes[12..16].copy_from_slice(&self.profile_count.to_le_bytes());
        bytes[16..20].copy_from_slice(&self.feature_count.to_le_bytes());
        bytes
    }

    fn decode(bytes: &[u8]) -> io::Result<Self> {
        if bytes.len() < HEADER_SIZE {
            return Err(invalid("profile store header is truncated"));
        }
        if !bytes.starts_with(&MAGIC[..]) {
            return Err(invalid("profile store magic mismatch"));
        }

        let version = read_u32(bytes, 8)?;
        if version != VERSION {
            return Err(invalid(format!(
                "unsupported profile store version: {version}"
            )));
        }

        let flags = read_u32(bytes, 20)?;
        if flags != 0 {
            return Err(invalid(format!(
                "unsupported profile store flags: {flags:#x}"
            )));
        }

        Ok(Self {
            profile_count: read_u32(bytes, 12)?,
            feature_count: read_u32(bytes, 16)?,
        })
    }
}

impl RecordIndex {
    fn encode(self) -> [u8; INDEX_SIZE] {
        let mut bytes = [0u8; INDEX_SIZE];
        bytes[..4].copy_from_slice(&self.key.to_le_bytes());
        bytes[4..12].copy_from_slice(&self.offset.to_le_bytes());
        bytes[12..].copy_from_slice(&self.len.to_le_bytes());
        bytes
    }

    fn decode(bytes: &[u8]) -> io::Result<Self> {
        if bytes.len() < INDEX_SIZE {
            return Err(invalid("profile store index is truncated"));
        }
        Ok(Self {
            key: read_u32(bytes, 0)?,
            offset: read_u64(bytes, 4)?,
            len: read_u32(bytes, 12)?,
        })
    }
}

fn save_lock() -> MutexGuard<'static, ()> {
    SAVE_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

fn invalid(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.into())
}

fn context(action: &str, error: io::Error) -> io::Error {
    io::Error::new(error.kind(), format!("{action} profile store: {error}"))
}

fn read_u32(bytes: &[u8], offset: usize) -> io::Result<u32> {
    let end = offset
        .checked_add(4)
        .ok_or_else(|| invalid("profile store offset overflow"))?;
    let raw = bytes
        .get(offset..end)
        .ok_or_else(|| invalid("profile store is truncated"))?;
    Ok(u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]))
}

fn read_u64(bytes: &[u8], offset: usize) -> io::Result<u64> {
    let end = offset
        .checked_add(8)
        .ok_or_else(|| invalid("profile store offset overflow"))?;
    let raw = bytes
        .get(offset..end)
        .ok_or_else(|| invalid("profile store is truncated"))?;
    Ok(u64::from_le_bytes([
        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
    ]))
}

pub(crate) fn read() -> io::Result<Store> {
    match fs::read(STORE_PATH) {
        Ok(bytes) => decode(&bytes),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(Store::default()),
        Err(e) => Err(context("read", e)),
    }
}

fn upsert<T, K>(entries: &mut Vec<T>, entry: &T, key: impl Fn(&T) -> K) -> bool
where
    T: Clone + PartialEq,
    K: PartialEq,
{
    let target = key(entry);
    let position = entries.iter().position(|slot| key(slot) == target);
    match position {
        Some(index) if &entries[index] == entry => false,
        Some(index) => {
            entries[index] = entry.clone();
            true
        }
        None => {
            entries.push(entry.clone());
            true
        }
    }
}

fn remove<T, K>(entries: &mut Vec<T>, key: K, key_of: impl Fn(&T) -> K) -> bool
where
    K: PartialEq,
{
    let before = entries.len();
    entries.retain(|entry| key_of(entry) != key);
    entries.len() != before
}

pub(crate) fn save_profile(entry: &ProfileEntry) -> io::Result<Option<usize>> {
    ensure_root()?;
    let _guard = save_lock();

    let mut store = read()?;
    if !upsert(&mut store.profiles, entry, |e| e.uid) {
        return Ok(None);
    }

    write_unlocked(&store)?;
    Ok(Some(store.profiles.len()))
}

pub(crate) fn del_profile(uid: u32) -> io::Result<Option<usize>> {
    ensure_root()?;
    let _guard = save_lock();

    let mut store = read()?;
    if !remove(&mut store.profiles, uid, |e| e.uid) {
        return Ok(None);
    }

    write_unlocked(&store)?;
    Ok(Some(store.profiles.len()))
}

pub(crate) fn save_feature(entry: &FeatureEntry) -> io::Result<Option<usize>> {
    ensure_root()?;
    let _guard = save_lock();

    let mut store = read()?;
    if !upsert(&mut store.features, entry, |e| e.id) {
        return Ok(None);
    }

    write_unlocked(&store)?;
    Ok(Some(store.features.len()))
}

pub(crate) fn del_feature(id: i32) -> io::Result<Option<usize>> {
    ensure_root()?;
    let _guard = save_lock();

    let mut store = read()?;
    if !remove(&mut store.features, id, |e| e.id) {
        return Ok(None);
    }

    write_unlocked(&store)?;
    Ok(Some(store.features.len()))
}

pub(crate) fn replace_features(features: Vec<FeatureEntry>) -> io::Result<()> {
    ensure_root()?;
    let _guard = save_lock();

    let mut store = read()?;
    store.features = features;
    write_unlocked(&store)
}

pub(crate) fn finish_load(label: &str, total: usize, failed: usize) -> io::Result<()> {
    if failed == 0 {
        log::info!("{label} loaded: {total} entries");
        return Ok(());
    }

    log::warn!(
        "{label} load done: {}/{total} succeeded, {failed} failed",
        total - failed,
    );
    Err(io::Error::other(format!(
        "{label} load: {failed}/{total} entries failed"
    )))
}

fn decode(bytes: &[u8]) -> io::Result<Store> {
    let header = Header::decode(bytes)?;
    let profile_count = header.profile_count as usize;
    let feature_count = header.feature_count as usize;

    let profile_index_bytes = profile_count
        .checked_mul(INDEX_SIZE)
        .ok_or_else(|| invalid("profile store index size overflow"))?;
    let feature_index_bytes = feature_count
        .checked_mul(INDEX_SIZE)
        .ok_or_else(|| invalid("profile store index size overflow"))?;
    let data_start = HEADER_SIZE
        .checked_add(profile_index_bytes)
        .and_then(|size| size.checked_add(feature_index_bytes))
        .ok_or_else(|| invalid("profile store size overflow"))?;
    if data_start > bytes.len() {
        return Err(invalid(format!(
            "profile store index is truncated: need {data_start} bytes, have {}",
            bytes.len()
        )));
    }

    let mut profiles = Vec::with_capacity(profile_count);
    for position in 0..profile_count {
        let offset = HEADER_SIZE + position * INDEX_SIZE;
        let index = RecordIndex::decode(&bytes[offset..offset + INDEX_SIZE])?;
        let payload = payload(bytes, index, data_start)?;
        let entry = ProfileEntry::from_bytes(payload).map_err(|e| {
            invalid(format!(
                "decode profile entry key={} offset={}: {e}",
                index.key, index.offset
            ))
        })?;
        if entry.uid != index.key {
            return Err(invalid(format!(
                "profile entry key mismatch: index={} entry={}",
                index.key, entry.uid
            )));
        }
        profiles.push(entry);
    }

    let feature_index_start = HEADER_SIZE + profile_index_bytes;
    let mut features = Vec::with_capacity(feature_count);
    for position in 0..feature_count {
        let offset = feature_index_start + position * INDEX_SIZE;
        let index = RecordIndex::decode(&bytes[offset..offset + INDEX_SIZE])?;
        let payload = payload(bytes, index, data_start)?;
        let entry = FeatureEntry::from_bytes(payload).map_err(|e| {
            invalid(format!(
                "decode feature entry key={} offset={}: {e}",
                index.key, index.offset
            ))
        })?;
        if entry.id as u32 != index.key {
            return Err(invalid(format!(
                "feature entry key mismatch: index={} entry={}",
                index.key, entry.id
            )));
        }
        features.push(entry);
    }

    Ok(Store { profiles, features })
}

fn payload<'a>(bytes: &'a [u8], index: RecordIndex, data_start: usize) -> io::Result<&'a [u8]> {
    let offset = usize::try_from(index.offset).map_err(|_| {
        invalid(format!(
            "profile store offset is too large: {}",
            index.offset
        ))
    })?;
    let len = index.len as usize;
    if offset < data_start {
        return Err(invalid(format!(
            "profile store entry key={} points into the index area",
            index.key
        )));
    }

    let end = offset.checked_add(len).ok_or_else(|| {
        invalid(format!(
            "profile store entry key={} range overflow",
            index.key
        ))
    })?;
    bytes.get(offset..end).ok_or_else(|| {
        invalid(format!(
            "profile store entry key={} is out of bounds: {offset}..{end}, file={}",
            index.key,
            bytes.len()
        ))
    })
}

fn encode(store: &Store) -> io::Result<Vec<u8>> {
    let profile_count =
        u32::try_from(store.profiles.len()).map_err(|_| invalid("too many profile entries"))?;
    let feature_count =
        u32::try_from(store.features.len()).map_err(|_| invalid("too many feature entries"))?;

    let profile_index_bytes = (profile_count as usize)
        .checked_mul(INDEX_SIZE)
        .ok_or_else(|| invalid("profile store index size overflow"))?;
    let feature_index_bytes = (feature_count as usize)
        .checked_mul(INDEX_SIZE)
        .ok_or_else(|| invalid("profile store index size overflow"))?;
    let data_start = HEADER_SIZE
        .checked_add(profile_index_bytes)
        .and_then(|size| size.checked_add(feature_index_bytes))
        .ok_or_else(|| invalid("profile store size overflow"))?;

    let header = Header {
        profile_count,
        feature_count,
    };
    let mut bytes = Vec::with_capacity(data_start);
    bytes.extend_from_slice(&header.encode());
    bytes.resize(data_start, 0);

    let feature_index_start = HEADER_SIZE + profile_index_bytes;
    let mut cursor = data_start;

    for (position, entry) in store.profiles.iter().enumerate() {
        let payload = entry.to_bytes();
        let len =
            u32::try_from(payload.len()).map_err(|_| invalid("profile entry is too large"))?;
        let offset = u64::try_from(cursor).map_err(|_| invalid("profile store is too large"))?;
        write_index(
            &mut bytes,
            HEADER_SIZE + position * INDEX_SIZE,
            RecordIndex {
                key: entry.uid,
                offset,
                len,
            },
        )?;
        bytes.extend_from_slice(&payload);
        cursor = cursor
            .checked_add(payload.len())
            .ok_or_else(|| invalid("profile store size overflow"))?;
    }

    for (position, entry) in store.features.iter().enumerate() {
        let payload = entry.to_bytes();
        let len =
            u32::try_from(payload.len()).map_err(|_| invalid("feature entry is too large"))?;
        let offset = u64::try_from(cursor).map_err(|_| invalid("profile store is too large"))?;
        write_index(
            &mut bytes,
            feature_index_start + position * INDEX_SIZE,
            RecordIndex {
                key: entry.id as u32,
                offset,
                len,
            },
        )?;
        bytes.extend_from_slice(&payload);
        cursor = cursor
            .checked_add(payload.len())
            .ok_or_else(|| invalid("profile store size overflow"))?;
    }

    Ok(bytes)
}

fn write_index(bytes: &mut [u8], position: usize, index: RecordIndex) -> io::Result<()> {
    let end = position
        .checked_add(INDEX_SIZE)
        .ok_or_else(|| invalid("profile store index offset overflow"))?;
    let slot = bytes
        .get_mut(position..end)
        .ok_or_else(|| invalid("profile store index is out of bounds"))?;
    slot.copy_from_slice(&index.encode());
    Ok(())
}

fn write_unlocked(store: &Store) -> io::Result<()> {
    let bytes = encode(store)?;
    persist(Path::new(STORE_PATH), &bytes)
}

fn persist(path: &Path, bytes: &[u8]) -> io::Result<()> {
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).map_err(|e| context("create directory for", e))?;
    }

    let tmp = temp_path(path);
    match fs::remove_file(&tmp) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(context("remove stale temp for", e)),
    }

    if let Err(e) = write_temp(&tmp, bytes).map_err(|e| context("write temp for", e)) {
        let _ = fs::remove_file(&tmp);
        log::error!("profile store: write temp failed: {e}");
        return Err(e);
    }

    if let Err(e) = fs::rename(&tmp, path).map_err(|e| context("replace", e)) {
        let _ = fs::remove_file(&tmp);
        log::error!("profile store: rename failed: {e}");
        return Err(e);
    }

    // The data is already durable after the rename; only report a failed
    // directory sync instead of turning a successful save into an error.
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        match File::open(parent) {
            Ok(dir) => {
                if let Err(e) = dir.sync_all() {
                    log::warn!("profile store: sync directory failed: {e}");
                }
            }
            Err(e) => log::warn!("profile store: open directory for sync failed: {e}"),
        }
    }
    Ok(())
}

fn write_temp(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }

    let mut file = options.open(path)?;
    file.write_all(bytes)?;
    file.sync_all()
}

fn temp_path(path: &Path) -> PathBuf {
    let mut name = path.as_os_str().to_os_string();
    name.push(".tmp");
    PathBuf::from(name)
}

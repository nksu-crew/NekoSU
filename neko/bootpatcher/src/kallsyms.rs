// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Extract the symbol names embedded in an AArch64 Linux kernel image.
 *
 * Android GKI kernels still carry the full kallsyms table even though most of
 * it is not exported to modules. The table is built by scripts/kallsyms.c and
 * laid out, in order, as:
 *
 *     kallsyms_num_syms      u32 symbol count
 *     kallsyms_names         compressed names, every 256th offset in markers
 *     kallsyms_markers       u32 offsets into kallsyms_names
 *     kallsyms_token_table   256 NUL-terminated substrings
 *     kallsyms_token_index   256 u16 offsets into the token table
 *
 * The image contains no ELF symbols for these arrays, so they are located by
 * structure: the token table always stores the ten digits as one-byte tokens
 * at indices '0'..'9' in ascending, contiguous order; the token index is a
 * strictly increasing u16 array; markers precede the token table; and the
 * symbol count is the u32 right before the 8-byte-aligned names array.
 *
 * Only names are needed. Addresses (kallsyms_offsets, kallsyms_relative_base)
 * are deliberately ignored, which keeps this independent of KASLR, pointer
 * size and CONFIG_KALLSYMS_BASE_RELATIVE.
 */

use std::collections::HashSet;

use anyhow::{Result, anyhow, bail};

/// Maximum distance between the start of kallsyms_names and kallsyms_markers.
/// Real kernels use a few megabytes; the cap bounds the scan on malformed input.
const MAX_NAMES_BYTES: usize = 32 * 1024 * 1024;

const MIN_SYMBOLS: usize = 256;
const MAX_SYMBOLS: usize = 4_000_000;

/// Decode every symbol name from a decompressed kernel image.
pub fn parse_names(kernel: &[u8]) -> Result<HashSet<String>> {
    let mut last_error = None;

    for digit_start in digit_token_candidates(kernel) {
        let Some(table_start) = token_table_start(kernel, digit_start) else {
            continue;
        };
        let Some((tokens, table_end)) = parse_token_table(kernel, table_start) else {
            continue;
        };
        let Some(_index_start) = find_token_index(kernel, table_end, &tokens) else {
            continue;
        };

        match find_names(kernel, table_start, &tokens) {
            Ok(names) => return Ok(names),
            Err(error) => last_error = Some(error),
        }
    }

    match last_error {
        Some(error) => Err(error.context("no valid kallsyms table found in the kernel image")),
        None => bail!(
            "no kallsyms token table found in the kernel image \
             (CONFIG_KALLSYMS disabled, or unsupported kernel layout)"
        ),
    }
}

fn digit_token_candidates(kernel: &[u8]) -> Vec<usize> {
    let needle: Vec<u8> = (b'0'..=b'9').flat_map(|digit| [digit, 0]).collect();

    let mut all = Vec::new();
    let mut preferred = Vec::new();
    let mut from = 0;
    while let Some(relative) = find(&kernel[from..], &needle) {
        let position = from + relative;
        all.push(position);
        // A real token table continues with another token. Digits inside a
        // string or another table usually end the sequence in punctuation.
        let after = position + needle.len();
        if kernel
            .get(after)
            .is_some_and(|byte| byte.is_ascii_alphanumeric() || *byte == b'_')
        {
            preferred.push(position);
        }
        from = position + 1;
    }

    if preferred.is_empty() { all } else { preferred }
}

/// Walk back over the 0x30 tokens that precede the digit tokens. Every token
/// starts after the previous NUL, so this lands exactly on token 0.
fn token_table_start(kernel: &[u8], digit_start: usize) -> Option<usize> {
    let mut position = digit_start.checked_sub(1)?;

    for _ in 0..0x30 {
        let mut stopped = false;
        for _ in 0..128 {
            position = position.checked_sub(1)?;
            let byte = *kernel.get(position)?;
            if byte == 0 || byte > b'z' {
                stopped = true;
                break;
            }
        }
        if !stopped {
            return None;
        }
    }

    Some(position + 1)
}

fn parse_token_table(kernel: &[u8], start: usize) -> Option<(Vec<&[u8]>, usize)> {
    let mut tokens = Vec::with_capacity(256);
    let mut position = start;

    for _ in 0..256 {
        let body_start = position;
        let mut body_end = body_start;
        while body_end < kernel.len() && body_end - body_start <= 128 && kernel[body_end] != 0 {
            if !(0x20..=0x7e).contains(&kernel[body_end]) {
                return None;
            }
            body_end += 1;
        }
        if body_end >= kernel.len() || kernel[body_end] != 0 || body_end - body_start > 128 {
            return None;
        }
        tokens.push(&kernel[body_start..body_end]);
        position = body_end + 1;
    }

    for (offset, digit) in (b'0'..=b'9').enumerate() {
        if tokens[0x30 + offset] != [digit] {
            return None;
        }
    }

    Some((tokens, position))
}

fn find_token_index(kernel: &[u8], table_end: usize, tokens: &[&[u8]]) -> Option<usize> {
    let mut offsets = Vec::with_capacity(256);
    let mut offset = 0usize;
    for token in tokens {
        offsets.push(u16::try_from(offset).ok()?);
        offset = offset.checked_add(token.len() + 1)?;
    }

    let mut packed = Vec::with_capacity(256 * 2);
    for offset in &offsets {
        packed.extend_from_slice(&offset.to_le_bytes());
    }

    let end = table_end.checked_add(packed.len() + 256)?.min(kernel.len());
    let relative = find(kernel.get(table_end..end)?, &packed)?;
    Some(table_end + relative)
}

fn find_names(
    kernel: &[u8],
    token_table_start: usize,
    tokens: &[&[u8]],
) -> Result<HashSet<String>> {
    let upper = token_table_start;
    let lower = upper.saturating_sub(MAX_NAMES_BYTES);
    let mut start = (lower + 7) & !7;

    while start + 8 <= upper {
        if let Some(names) = try_names_at(kernel, start, upper, tokens) {
            return Ok(names);
        }
        start += 8;
    }

    Err(anyhow!(
        "kallsyms_names/kallsyms_markers not found before the token table"
    ))
}

fn try_names_at(
    kernel: &[u8],
    start: usize,
    token_table_start: usize,
    tokens: &[&[u8]],
) -> Option<HashSet<String>> {
    let count = read_u32(kernel, start.checked_sub(8)?)? as usize;
    if !(MIN_SYMBOLS..=MAX_SYMBOLS).contains(&count) {
        return None;
    }

    let blocks = count.div_ceil(256);
    let markers_bytes = blocks.checked_mul(4)?;

    // Depending on the kernel version, kallsyms_seqs_of_names (3 bytes per
    // symbol) either sits between the markers and the token table (stable
    // backports) or after the address tables (6.4+). Try both layouts.
    let without_seqs = token_table_start.checked_sub(align8(markers_bytes)?);
    let seqs_bytes = count.checked_mul(3)?;
    let with_seqs =
        token_table_start.checked_sub(align8(align8(markers_bytes)?.checked_add(seqs_bytes)?)?);

    for markers_start in [without_seqs, with_seqs].into_iter().flatten() {
        if let Some(names) = try_layout(kernel, start, markers_start, count, blocks, tokens) {
            return Some(names);
        }
    }

    None
}

fn try_layout(
    kernel: &[u8],
    start: usize,
    markers_start: usize,
    count: usize,
    blocks: usize,
    tokens: &[&[u8]],
) -> Option<HashSet<String>> {
    if markers_start <= start {
        return None;
    }
    let names_size = markers_start - start;
    if names_size > MAX_NAMES_BYTES {
        return None;
    }
    // Every record needs at least a length byte and one token byte.
    if names_size < count.checked_mul(2)? {
        return None;
    }

    let mut markers = Vec::with_capacity(blocks);
    let mut previous = 0usize;
    for block in 0..blocks {
        let offset = read_u32(kernel, markers_start.checked_add(block * 4)?)? as usize;
        if block == 0 {
            if offset != 0 {
                return None;
            }
        } else {
            let delta = offset.checked_sub(previous)?;
            // 256 records: at least 2 bytes each, at most ~0x4000 bytes each.
            if !(0x200..=0x40000).contains(&delta) {
                return None;
            }
        }
        markers.push(offset);
        previous = offset;
    }

    let mut position = 0usize;
    let mut names = HashSet::with_capacity(count);

    for (block, &marker) in markers.iter().enumerate() {
        if position != marker {
            return None;
        }
        let records = 256.min(count - block * 256);
        for _ in 0..records {
            let (length, header) = read_uleb128(kernel, start + position)?;
            if length == 0 || length > 0x3fff {
                return None;
            }
            let body_start = start + position + header;
            let body_end = body_start.checked_add(length)?;
            if body_end > markers_start {
                return None;
            }

            let mut name = Vec::with_capacity(length * 2);
            for &token in &kernel[body_start..body_end] {
                name.extend_from_slice(tokens.get(token as usize)?);
            }
            if name.len() < 2 || !is_symbol_type(name[0]) {
                return None;
            }
            let name = std::str::from_utf8(&name[1..]).ok()?;
            names.insert(name.to_owned());

            position = body_end - start;
        }
    }

    // The names array is 8-byte aligned, so only padding may follow the last
    // record before kallsyms_markers.
    if position > names_size || names_size - position >= 8 {
        return None;
    }

    Some(names)
}

fn read_uleb128(data: &[u8], position: usize) -> Option<(usize, usize)> {
    let first = *data.get(position)?;
    if first & 0x80 == 0 {
        return Some((first as usize, 1));
    }
    let second = *data.get(position + 1)?;
    Some((((first & 0x7f) as usize) | ((second as usize) << 7), 2))
}

fn align8(value: usize) -> Option<usize> {
    value.checked_add(7).map(|value| value & !7)
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        data.get(offset..offset + 4)?.try_into().ok()?,
    ))
}

fn is_symbol_type(byte: u8) -> bool {
    byte.is_ascii_alphabetic() || matches!(byte, b'?' | b'-')
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Image {
        data: Vec<u8>,
    }

    impl Image {
        fn put(&mut self, offset: usize, bytes: &[u8]) {
            if self.data.len() < offset + bytes.len() {
                self.data.resize(offset + bytes.len(), 0);
            }
            self.data[offset..offset + bytes.len()].copy_from_slice(bytes);
        }

        fn align(&mut self, alignment: usize) {
            let len = self.data.len();
            let padded = align8(len).unwrap();
            let padded = (padded + alignment - 1) & !(alignment - 1);
            self.data.resize(padded, 0);
        }
    }

    fn synthetic_kernel(names: &[&str], with_seqs: bool) -> Vec<u8> {
        let mut image = Image {
            data: vec![0; 0x100],
        };

        // Build a token table whose indices are the printable-ASCII codes; the
        // encoder below uses each character as its own token. Indices that the
        // names never use get a filler token, like scripts/kallsyms.c does.
        let mut used = [false; 256];
        for name in names {
            for byte in name.bytes() {
                used[byte as usize] = true;
            }
        }
        for digit in b'0'..=b'9' {
            used[digit as usize] = true;
        }

        let mut token_table = Vec::new();
        let mut offsets = Vec::new();
        for index in 0..=255u8 {
            offsets.push(token_table.len() as u16);
            let token = if used[index as usize] {
                vec![index]
            } else {
                b"ab".to_vec()
            };
            token_table.extend_from_slice(&token);
            token_table.push(0);
        }

        let mut encoded = Vec::new();
        let mut markers = Vec::new();
        for (index, name) in names.iter().enumerate() {
            if index % 256 == 0 {
                markers.push(encoded.len() as u32);
            }
            let body = name.as_bytes();
            assert!(body.len() <= 0x7f);
            encoded.push(body.len() as u8);
            encoded.extend_from_slice(body);
        }

        image.put(0x100, &(names.len() as u32).to_le_bytes());
        image.align(8);
        let names_start = image.data.len();
        image.put(names_start, &encoded);
        image.align(8);
        let markers_start = image.data.len();
        for marker in &markers {
            image.put(image.data.len(), &marker.to_le_bytes());
        }
        image.align(8);
        if with_seqs {
            // 3 bytes per symbol, as in kallsyms_seqs_of_names.
            for index in 0..names.len() * 3 {
                image.put(image.data.len(), &[(index % 251) as u8]);
            }
            image.align(8);
        }
        let token_table_start = image.data.len();
        image.put(token_table_start, &token_table);
        image.align(8);
        let index_start = image.data.len();
        for offset in &offsets {
            image.put(image.data.len(), &offset.to_le_bytes());
        }
        image.put(image.data.len(), b"tail");

        // The parser derives markers_start from the symbol count and the token
        // table position; make sure the fixture matches that invariant.
        let expected = if with_seqs {
            token_table_start
                - align8(align8(markers.len() * 4).unwrap() + names.len() * 3).unwrap()
        } else {
            token_table_start - align8(markers.len() * 4).unwrap()
        };
        assert_eq!(markers_start, expected);
        assert_eq!(
            index_start,
            align8(token_table_start + token_table.len()).unwrap()
        );

        image.data
    }

    fn sample_names() -> Vec<String> {
        (0..1000)
            .map(|index| format!("Tksym_{index}"))
            .chain(["Dselinux_state".into(), "Tkallsyms_lookup_name".into()])
            .collect()
    }

    #[test]
    fn parses_table_without_seqs() {
        let names = sample_names();
        let refs: Vec<&str> = names.iter().map(String::as_str).collect();
        let kernel = synthetic_kernel(&refs, false);

        let parsed = parse_names(&kernel).unwrap();
        assert_eq!(parsed.len(), names.len());
        assert!(parsed.contains("ksym_999"));
        assert!(parsed.contains("selinux_state"));
        assert!(parsed.contains("kallsyms_lookup_name"));
    }

    #[test]
    fn parses_table_with_seqs() {
        let names = sample_names();
        let refs: Vec<&str> = names.iter().map(String::as_str).collect();
        let kernel = synthetic_kernel(&refs, true);

        let parsed = parse_names(&kernel).unwrap();
        assert_eq!(parsed.len(), names.len());
        assert!(parsed.contains("ksym_999"));
        assert!(parsed.contains("kallsyms_lookup_name"));
    }

    #[test]
    fn rejects_image_without_kallsyms() {
        let error = parse_names(&vec![0u8; 0x2000]).unwrap_err();
        assert!(error.to_string().contains("no kallsyms token table"));
    }
}

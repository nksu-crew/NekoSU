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
 *     kallsyms_markers       u32 offsets into kallsyms_names (u64 before Linux 4.20)
 *     kallsyms_token_table   256 NUL-terminated substrings
 *     kallsyms_token_index   256 u16 offsets into the token table
 *
 * Some stable kernels also carry kallsyms_seqs_of_names (3 bytes per symbol)
 * between the markers and the token table; newer mainline kernels put it after
 * the address tables. Only names are needed here, so both layouts are handled
 * by walking backwards from the token table and accepting either a marker
 * array that ends at the token table or one followed by the 3-byte seqs array.
 *
 * The image contains no ELF symbols for these arrays, so they are located by
 * structure, following the same approach as KernelPatch's tools/kallsym.c:
 *
 *   - the token table always stores the ten digits as one-byte tokens at
 *     indices '0'..'9' in ascending, contiguous order;
 *   - the first markers entry is zero and the entries are strictly increasing
 *     offsets into the names stream;
 *   - the names stream is validated against several 256-symbol marker
 *     boundaries before it is decoded.
 *
 * Addresses (kallsyms_offsets, kallsyms_relative_base) are deliberately
 * ignored, which keeps this independent of KASLR, pointer size and
 * CONFIG_KALLSYMS_BASE_RELATIVE.
 */

use std::collections::HashSet;

use anyhow::{Context, Result, anyhow, bail};

/// Maximum distance from the token table back to kallsyms_markers. Real
/// kernels use a few megabytes; the cap bounds the scan on malformed input.
const MAX_LAYOUT_BYTES: usize = 64 * 1024 * 1024;

/// Marker arrays have one entry per 256 symbols. Android kernels have far more
/// than this, but keeping a low bound also lets small test images work.
const MIN_MARKERS: usize = 4;
/// 4M symbols is already far beyond any real kernel.
const MAX_MARKERS: usize = 16_384;

/// Number of marker boundaries checked while looking for kallsyms_names.
const VERIFY_MARKERS: usize = 5;

/// The kernel encodes a symbol with at most KSYM_NAME_LEN token bytes.
const MAX_ENCODED_NAME: usize = 512;
const MAX_SYMBOL_NAME: usize = 512;
const MAX_TOKEN_LEN: usize = 512;

struct TokenTable<'a> {
    tokens: Vec<&'a [u8]>,
    table_start: usize,
}

struct Markers {
    offset: usize,
    values: Vec<usize>,
}

#[derive(Clone, Copy)]
enum MarkerLayout {
    NoSeqs,
    WithSeqs,
}

/// Decode every symbol name from a decompressed kernel image.
pub fn parse_names(kernel: &[u8]) -> Result<HashSet<String>> {
    let table = find_token_table(kernel)?;
    let markers = find_markers(kernel, table.table_start)
        .ok_or_else(|| anyhow!("kallsyms_markers not found before the token table"))?;
    let names_start = find_names_start(kernel, &markers)
        .ok_or_else(|| anyhow!("kallsyms_names not found before the markers"))?;
    decode_names(kernel, names_start, &markers, &table.tokens)
}

fn digit_token_candidates(kernel: &[u8]) -> Vec<usize> {
    let needle: Vec<u8> = (b'0'..=b'9').flat_map(|digit| [digit, 0]).collect();

    let mut candidates = Vec::new();
    let mut from = 0;
    while let Some(relative) = find(&kernel[from..], &needle) {
        let position = from + relative;
        candidates.push(position);
        from = position + 1;
    }
    candidates
}

/// Walk back over the 0x30 tokens that precede the digit tokens. Every token
/// starts after the previous NUL, so this lands exactly on token 0.
fn token_table_start(kernel: &[u8], digit_start: usize) -> Option<usize> {
    let mut position = digit_start.checked_sub(1)?;

    for _ in 0..0x30 {
        let mut stopped = false;
        for _ in 0..MAX_TOKEN_LEN {
            position = position.checked_sub(1)?;
            let byte = *kernel.get(position)?;
            if byte == 0 {
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
        while body_end < kernel.len()
            && body_end - body_start <= MAX_TOKEN_LEN
            && kernel[body_end] != 0
        {
            if !(0x20..=0x7e).contains(&kernel[body_end]) {
                return None;
            }
            body_end += 1;
        }
        if body_end >= kernel.len()
            || kernel[body_end] != 0
            || body_end - body_start > MAX_TOKEN_LEN
        {
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

/// Real token tables contain the lowercase letters as one-byte tokens at their
/// ASCII indices. This rejects most digit sequences found in unrelated data.
fn has_letter_tokens(tokens: &[&[u8]]) -> bool {
    (0..10).all(|offset| {
        tokens
            .get(0x61 + offset)
            .is_some_and(|token| token.len() == 1 && token[0] == b'a' + offset as u8)
    })
}

fn try_token_table(
    kernel: &[u8],
    digit_start: usize,
    require_letters: bool,
) -> Option<TokenTable<'_>> {
    let start = token_table_start(kernel, digit_start)?;
    let (tokens, table_end) = parse_token_table(kernel, start)?;
    if require_letters && !has_letter_tokens(&tokens) {
        return None;
    }
    find_token_index(kernel, table_end, &tokens)?;

    Some(TokenTable {
        tokens,
        table_start: start,
    })
}

fn find_token_table(kernel: &[u8]) -> Result<TokenTable<'_>> {
    let mut fallback = None;

    for digit_start in digit_token_candidates(kernel) {
        if let Some(table) = try_token_table(kernel, digit_start, true) {
            return Ok(table);
        }
        if fallback.is_none() {
            fallback = try_token_table(kernel, digit_start, false);
        }
    }

    fallback.ok_or_else(|| {
        anyhow!(
            "no kallsyms token table found in the kernel image \
             (CONFIG_KALLSYMS disabled, or unsupported kernel layout)"
        )
    })
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

/// Locate the marker array by scanning backwards from the token table. The
/// marker array is 8-byte aligned and starts with a zero entry; its distance to
/// the token table determines the number of entries for both known layouts.
fn find_markers(kernel: &[u8], token_table_start: usize) -> Option<Markers> {
    let lower = token_table_start.saturating_sub(MAX_LAYOUT_BYTES);
    let mut marker_start = token_table_start.checked_sub(8)? & !7;

    loop {
        if marker_start < lower {
            break;
        }
        if read_u32(kernel, marker_start) == Some(0) {
            for elem_size in [4usize, 8] {
                if let Some(markers) =
                    try_markers_at(kernel, marker_start, token_table_start, elem_size)
                {
                    return Some(markers);
                }
            }
        }
        if marker_start < 8 {
            break;
        }
        marker_start -= 8;
    }

    None
}

fn try_markers_at(
    kernel: &[u8],
    marker_start: usize,
    token_table_start: usize,
    elem_size: usize,
) -> Option<Markers> {
    let distance = token_table_start.checked_sub(marker_start)?;
    let max_count = (distance / elem_size).min(MAX_MARKERS);
    if max_count < MIN_MARKERS {
        return None;
    }

    let mut values = Vec::new();
    let mut position = marker_start;
    while values.len() < max_count {
        let Some(value) = read_marker(kernel, position, elem_size) else {
            break;
        };
        if let Some(previous) = values.last() {
            if value <= *previous {
                break;
            }
        }
        if value >= marker_start {
            break;
        }
        values.push(value);
        position = position.checked_add(elem_size)?;
    }

    if values.len() < MIN_MARKERS || values[0] != 0 {
        return None;
    }

    // The next entry after a real marker array is padding, seqs data or the
    // token table, so the forward scan normally stops at the exact count. If
    // the first byte after it happens to look like another increasing marker,
    // try the few possible truncations.
    let first = values.len().saturating_sub(4).max(MIN_MARKERS);
    for count in (first..=values.len()).rev() {
        let values = &values[..count];
        let Some(layout) = marker_layout(count, distance, elem_size) else {
            continue;
        };
        let markers_bytes = count.checked_mul(elem_size)?;
        let markers_end = marker_start.checked_add(markers_bytes)?;
        if markers_end > token_table_start {
            continue;
        }

        match layout {
            MarkerLayout::NoSeqs => {
                if !padding_zeros(kernel, markers_end, token_table_start) {
                    continue;
                }
            }
            MarkerLayout::WithSeqs => {
                let aligned_end = marker_start.checked_add(align8(markers_bytes)?)?;
                if aligned_end > token_table_start
                    || !padding_zeros(kernel, markers_end, aligned_end)
                {
                    continue;
                }
            }
        }

        return Some(Markers {
            offset: marker_start,
            values: values.to_vec(),
        });
    }

    None
}

fn marker_layout(count: usize, distance: usize, elem_size: usize) -> Option<MarkerLayout> {
    let markers_bytes = count.checked_mul(elem_size)?;
    let aligned = align8(markers_bytes)?;

    if distance == aligned {
        return Some(MarkerLayout::NoSeqs);
    }

    let remainder = distance.checked_sub(aligned)?;
    let min_symbols = count.checked_sub(1)?.checked_mul(256)?;
    let max_symbols = count.checked_mul(256)?;

    for padding in 0..8 {
        let Some(seqs_bytes) = remainder.checked_sub(padding) else {
            continue;
        };
        if seqs_bytes % 3 != 0 {
            continue;
        }
        let symbols = seqs_bytes / 3;
        if symbols > min_symbols
            && symbols <= max_symbols
            && align8(aligned.checked_add(seqs_bytes)?)? == distance
        {
            return Some(MarkerLayout::WithSeqs);
        }
    }

    None
}

fn find_names_start(kernel: &[u8], markers: &Markers) -> Option<usize> {
    let last_marker = *markers.values.last()?;
    let guess = markers.offset.checked_sub(last_marker)?;

    // The last block is at most 256 records of MAX_ENCODED_NAME bytes, so the
    // real start lies at most 128 KiB before the guess derived from the last
    // marker. Check that small window first, then fall back to a full scan.
    let window_start = guess.saturating_sub(0x20000);
    let window_end = markers.offset.min(guess.saturating_add(0x1000));
    let mut candidate = align_up8(window_start);
    while candidate < window_end {
        if verify_names_candidate(kernel, candidate, markers) {
            return Some(candidate);
        }
        candidate = candidate.checked_add(8)?;
    }

    let mut candidate = 0usize;
    while candidate + 8 <= markers.offset {
        if verify_names_candidate(kernel, candidate, markers) {
            return Some(candidate);
        }
        candidate += 8;
    }

    None
}

/// Validate a possible kallsyms_names start against the first few marker
/// boundaries. This only reads encoded record lengths, so it is cheap.
fn verify_names_candidate(kernel: &[u8], start: usize, markers: &Markers) -> bool {
    if start >= markers.offset {
        return false;
    }
    let target = VERIFY_MARKERS.min(markers.values.len().saturating_sub(1));
    if target == 0 {
        return false;
    }

    let mut position = start;
    let mut records = 0usize;
    while position < markers.offset {
        let Some((length, header)) = read_uleb128(kernel, position) else {
            return false;
        };
        if length == 0 || length >= MAX_ENCODED_NAME {
            return false;
        }
        let Some(end) = position
            .checked_add(header)
            .and_then(|position| position.checked_add(length))
        else {
            return false;
        };
        if end > markers.offset {
            return false;
        }
        position = end;
        records += 1;

        if records % 256 == 0 {
            let marker_index = records / 256;
            let Some(&expected) = markers.values.get(marker_index) else {
                return false;
            };
            if position - start != expected {
                return false;
            }
            if marker_index >= target {
                return true;
            }
        }
    }

    false
}

fn decode_names(
    kernel: &[u8],
    names_start: usize,
    markers: &Markers,
    tokens: &[&[u8]],
) -> Result<HashSet<String>> {
    let mut names = HashSet::new();
    let mut position = names_start;
    let mut records = 0usize;
    let mut next_marker = 1usize;

    while position < markers.offset {
        // A zero length is not a symbol; it is the alignment padding after the
        // last record.
        if kernel[position] == 0 {
            break;
        }

        let (length, header) = read_uleb128(kernel, position)
            .with_context(|| format!("truncated symbol length at {position:#x}"))?;
        if length == 0 || length >= MAX_ENCODED_NAME {
            bail!("invalid symbol length {length} at {position:#x}");
        }
        let body_start = position + header;
        let body_end = body_start
            .checked_add(length)
            .with_context(|| format!("symbol at {position:#x} overflows"))?;
        if body_end > markers.offset {
            bail!("symbol at {position:#x} crosses kallsyms_markers");
        }

        if records > 0 && records % 256 == 0 {
            let Some(&expected) = markers.values.get(next_marker) else {
                bail!("kallsyms_names has more blocks than kallsyms_markers");
            };
            if position - names_start != expected {
                bail!(
                    "kallsyms_names block {} starts at {:#x}, expected {:#x}",
                    next_marker,
                    position - names_start,
                    expected
                );
            }
            next_marker += 1;
        }

        let mut name = Vec::with_capacity(length * 2);
        for &token in &kernel[body_start..body_end] {
            name.extend_from_slice(
                tokens
                    .get(token as usize)
                    .with_context(|| format!("invalid token {token} at {position:#x}"))?,
            );
        }

        if name.len() < 2 || !is_symbol_type(name[0]) {
            bail!("invalid symbol name at {position:#x}");
        }
        if name.len() > MAX_SYMBOL_NAME {
            bail!("symbol name at {position:#x} is too long");
        }
        let name = std::str::from_utf8(&name[1..])
            .with_context(|| format!("non-UTF-8 symbol name at {position:#x}"))?;
        names.insert(name.to_owned());

        position = body_end;
        records += 1;
    }

    if next_marker != markers.values.len() {
        bail!(
            "kallsyms_markers has {} blocks but the names table ended after {next_marker}",
            markers.values.len()
        );
    }
    let padding = markers.offset - position;
    if padding >= 8 {
        bail!("kallsyms_names ends {padding} bytes before kallsyms_markers");
    }
    if !kernel[position..markers.offset]
        .iter()
        .all(|byte| *byte == 0)
    {
        bail!("nonzero padding between kallsyms_names and kallsyms_markers");
    }

    Ok(names)
}

fn read_marker(data: &[u8], position: usize, elem_size: usize) -> Option<usize> {
    match elem_size {
        4 => read_u32(data, position).map(|value| value as usize),
        8 => {
            let bytes = data.get(position..position.checked_add(8)?)?;
            Some(u64::from_le_bytes(bytes.try_into().ok()?) as usize)
        }
        _ => None,
    }
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

fn align_up8(value: usize) -> usize {
    (value + 7) & !7
}

fn padding_zeros(data: &[u8], start: usize, end: usize) -> bool {
    if start > end {
        return false;
    }
    data.get(start..end)
        .is_some_and(|padding| padding.iter().all(|byte| *byte == 0))
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        data.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
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

    fn synthetic_kernel(names: &[&str], with_seqs: bool, marker_elem_size: usize) -> Vec<u8> {
        let mut image = Image {
            data: vec![0; 0x100],
        };

        // Build a token table whose indices are the printable-ASCII codes; the
        // encoder below uses each character as its own token. Include the
        // lowercase letters so the token-table validation sees the same
        // digit/letter pattern as a real kernel.
        let mut used = [false; 256];
        for name in names {
            for byte in name.bytes() {
                used[byte as usize] = true;
            }
        }
        for digit in b'0'..=b'9' {
            used[digit as usize] = true;
        }
        for letter in b'a'..=b'z' {
            used[letter as usize] = true;
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
            match marker_elem_size {
                4 => image.put(image.data.len(), &(*marker as u32).to_le_bytes()),
                8 => image.put(image.data.len(), &(*marker as u64).to_le_bytes()),
                _ => unreachable!(),
            }
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

        // The parser derives the layout from the distance between the markers
        // and the token table; make sure the fixture matches that invariant.
        let markers_bytes = markers.len() * marker_elem_size;
        let expected = if with_seqs {
            token_table_start - align8(align8(markers_bytes).unwrap() + names.len() * 3).unwrap()
        } else {
            token_table_start - align8(markers_bytes).unwrap()
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
        let kernel = synthetic_kernel(&refs, false, 4);

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
        let kernel = synthetic_kernel(&refs, true, 4);

        let parsed = parse_names(&kernel).unwrap();
        assert_eq!(parsed.len(), names.len());
        assert!(parsed.contains("ksym_999"));
        assert!(parsed.contains("kallsyms_lookup_name"));
    }

    #[test]
    fn parses_table_with_64bit_markers() {
        let names = sample_names();
        let refs: Vec<&str> = names.iter().map(String::as_str).collect();
        let kernel = synthetic_kernel(&refs, false, 8);

        let parsed = parse_names(&kernel).unwrap();
        assert_eq!(parsed.len(), names.len());
        assert!(parsed.contains("kallsyms_lookup_name"));
    }

    #[test]
    fn rejects_image_without_kallsyms() {
        let error = parse_names(&vec![0u8; 0x2000]).unwrap_err();
        assert!(error.to_string().contains("no kallsyms token table"));
    }
}

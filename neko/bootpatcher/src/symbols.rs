// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Read the kernel symbols nksu.ko resolves at load time.
 *
 * kernel/symbol/symbol.h records every kallsyms lookup in the custom
 * `.nksu_ksymtab` section. Each entry is a flags byte followed by a
 * NUL-terminated symbol name:
 *
 *     [0x01]["alloc_uid\0"][0x00]["security_dump_masked_av\0"]...
 *
 * Bit 0 marks a symbol the module cannot live without; the other entries are
 * best-effort lookups whose absence the module already tolerates.
 */

use std::collections::BTreeSet;

use anyhow::{Context, Result, bail};

pub const KSYMTAB_SECTION: &str = ".nksu_ksymtab";

const REQUIRED: u8 = 1;

const ELF_HEADER_SIZE: usize = 64;
const SECTION_HEADER_SIZE: usize = 64;
const ET_REL: u16 = 1;
const EM_AARCH64: u16 = 183;
const SHN_XINDEX: u16 = 0xffff;
const SHT_PROGBITS: u32 = 1;

/// Symbols the module resolves through kallsyms, split by whether the module
/// can still work when the target kernel does not provide them.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct SymbolTable {
    pub required: Vec<String>,
    pub optional: Vec<String>,
}

impl SymbolTable {
    pub fn is_empty(&self) -> bool {
        self.required.is_empty() && self.optional.is_empty()
    }
}

/// Extract the exported symbol table from a relocatable nksu.ko.
pub fn read(module: &[u8]) -> Result<SymbolTable> {
    let section = find_section(module, KSYMTAB_SECTION)?
        .with_context(|| format!("module has no {KSYMTAB_SECTION} section"))?;
    parse_entries(&section)
}

fn parse_entries(section: &[u8]) -> Result<SymbolTable> {
    let mut required = BTreeSet::new();
    let mut optional = BTreeSet::new();
    let mut offset = 0;

    while offset < section.len() {
        let flags = section[offset];
        offset += 1;
        if flags & !REQUIRED != 0 {
            bail!(
                "unknown {KSYMTAB_SECTION} flags 0x{flags:02x} at entry {}",
                required.len() + optional.len()
            );
        }

        let end = section[offset..]
            .iter()
            .position(|byte| *byte == 0)
            .map(|end| offset + end)
            .with_context(|| format!("unterminated symbol name in {KSYMTAB_SECTION}"))?;
        let name = &section[offset..end];
        offset = end + 1;

        if name.is_empty() {
            // A linker may pad the section to its alignment; trailing NULs
            // are not another (nameless) entry.
            if section[offset..].iter().all(|byte| *byte == 0) {
                break;
            }
            bail!("empty symbol name in {KSYMTAB_SECTION}");
        }
        if !name.iter().all(|byte| (0x21..=0x7e).contains(byte)) {
            bail!(
                "invalid symbol name in {KSYMTAB_SECTION}: {}",
                String::from_utf8_lossy(name)
            );
        }

        let name = String::from_utf8(name.to_vec())
            .with_context(|| format!("non-UTF-8 symbol name in {KSYMTAB_SECTION}"))?;
        if flags & REQUIRED != 0 {
            optional.remove(&name);
            required.insert(name);
        } else if !required.contains(&name) {
            optional.insert(name);
        }
    }

    Ok(SymbolTable {
        required: required.into_iter().collect(),
        optional: optional.into_iter().collect(),
    })
}

/// Return the contents of the named section of an ELF64 little-endian module.
fn find_section(elf: &[u8], wanted: &str) -> Result<Option<Vec<u8>>> {
    if elf.len() < ELF_HEADER_SIZE || &elf[..4] != b"\x7fELF" {
        bail!("not an ELF file");
    }
    if elf[4] != 2 {
        bail!("module is not ELF64");
    }
    if elf[5] != 1 {
        bail!("module is not little-endian");
    }

    let e_type = read_u16(elf, 16)?;
    if e_type != ET_REL {
        bail!("module is not a relocatable object (e_type={e_type})");
    }
    let e_machine = read_u16(elf, 18)?;
    if e_machine != EM_AARCH64 {
        bail!("module is not AArch64 (e_machine={e_machine})");
    }

    let e_shoff = read_u64(elf, 40)?;
    let e_shentsize = read_u16(elf, 58)?;
    let mut e_shnum = read_u16(elf, 60)?;
    let mut e_shstrndx = read_u16(elf, 62)?;

    if e_shentsize as usize != SECTION_HEADER_SIZE {
        bail!("unexpected section header size {e_shentsize}");
    }
    let shoff = usize::try_from(e_shoff).context("section table offset overflow")?;
    if shoff == 0 {
        bail!("module has no section table");
    }

    // ELF extended numbering: the real values live in section header 0.
    if e_shnum == 0 || e_shstrndx == SHN_XINDEX {
        let first = section_header(elf, shoff, SECTION_HEADER_SIZE)?;
        if e_shnum == 0 {
            e_shnum = u16::try_from(first.sh_size).context("too many sections")?;
        }
        if e_shstrndx == SHN_XINDEX {
            e_shstrndx = u16::try_from(first.sh_link).context("invalid section name table")?;
        }
    }

    let shnum = e_shnum as usize;
    if shnum == 0 {
        bail!("module has no sections");
    }
    ensure_table(elf, shoff, shnum, SECTION_HEADER_SIZE)?;

    let shstr = section_header_at(elf, shoff, SECTION_HEADER_SIZE, e_shstrndx as usize)?;
    let names = section_data(elf, &shstr)?;

    let mut found: Option<Vec<u8>> = None;
    for index in 0..shnum {
        let header = section_header_at(elf, shoff, SECTION_HEADER_SIZE, index)?;
        let name = section_name(names, header.sh_name)?;
        if name != wanted {
            continue;
        }
        if header.sh_type != SHT_PROGBITS {
            bail!("{wanted} is not a PROGBITS section");
        }
        let data = section_data(elf, &header)?;
        // The linker usually merges same-named input sections, but concatenate
        // defensively so a build that produces several still reads completely.
        match &mut found {
            Some(existing) => existing.extend_from_slice(data),
            None => found = Some(data.to_vec()),
        }
    }

    Ok(found)
}

#[derive(Debug)]
struct SectionHeader {
    sh_name: u32,
    sh_type: u32,
    sh_offset: u64,
    sh_size: u64,
    sh_link: u32,
}

fn section_header(elf: &[u8], table: usize, entry_size: usize) -> Result<SectionHeader> {
    section_header_at(elf, table, entry_size, 0)
}

fn section_header_at(
    elf: &[u8],
    table: usize,
    entry_size: usize,
    index: usize,
) -> Result<SectionHeader> {
    let offset = table
        .checked_add(
            index
                .checked_mul(entry_size)
                .context("section index overflow")?,
        )
        .context("section header offset overflow")?;
    let end = offset
        .checked_add(SECTION_HEADER_SIZE)
        .context("section header end overflow")?;
    let data = elf
        .get(offset..end)
        .with_context(|| format!("section header {index} is out of bounds"))?;

    Ok(SectionHeader {
        sh_name: read_u32(data, 0)?,
        sh_type: read_u32(data, 4)?,
        sh_offset: read_u64(data, 24)?,
        sh_size: read_u64(data, 32)?,
        sh_link: read_u32(data, 40)?,
    })
}

fn section_data<'a>(elf: &'a [u8], header: &SectionHeader) -> Result<&'a [u8]> {
    let start = usize::try_from(header.sh_offset).context("section offset overflow")?;
    let size = usize::try_from(header.sh_size).context("section size overflow")?;
    let end = start.checked_add(size).context("section end overflow")?;
    elf.get(start..end)
        .with_context(|| format!("section at {start:#x} (+{size:#x}) is out of bounds"))
}

fn section_name(names: &[u8], offset: u32) -> Result<&str> {
    let start = offset as usize;
    let end = names
        .get(start..)
        .with_context(|| format!("section name offset {offset} is out of bounds"))?
        .iter()
        .position(|byte| *byte == 0)
        .map(|end| start + end)
        .with_context(|| format!("section name at {offset} is not terminated"))?;
    std::str::from_utf8(&names[start..end]).context("invalid section name")
}

fn ensure_table(elf: &[u8], table: usize, count: usize, entry_size: usize) -> Result<()> {
    let end = table
        .checked_add(
            count
                .checked_mul(entry_size)
                .context("section table overflow")?,
        )
        .context("section table overflow")?;
    if end > elf.len() {
        bail!("section table extends past the end of the module");
    }
    Ok(())
}

fn read_u16(data: &[u8], offset: usize) -> Result<u16> {
    let bytes = data
        .get(offset..offset + 2)
        .with_context(|| format!("truncated u16 at {offset}"))?;
    Ok(u16::from_le_bytes(bytes.try_into().unwrap()))
}

fn read_u32(data: &[u8], offset: usize) -> Result<u32> {
    let bytes = data
        .get(offset..offset + 4)
        .with_context(|| format!("truncated u32 at {offset}"))?;
    Ok(u32::from_le_bytes(bytes.try_into().unwrap()))
}

fn read_u64(data: &[u8], offset: usize) -> Result<u64> {
    let bytes = data
        .get(offset..offset + 8)
        .with_context(|| format!("truncated u64 at {offset}"))?;
    Ok(u64::from_le_bytes(bytes.try_into().unwrap()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_entries_and_flags() {
        let mut section = Vec::new();
        for (flags, name) in [
            (1u8, "alloc_uid"),
            (0, "security_dump_masked_av"),
            (1, "alloc_uid"),
            (0, "write_op"),
            (1, "write_op"),
        ] {
            section.push(flags);
            section.extend_from_slice(name.as_bytes());
            section.push(0);
        }

        let table = parse_entries(&section).unwrap();
        assert_eq!(table.required, ["alloc_uid", "write_op"]);
        assert_eq!(table.optional, ["security_dump_masked_av"]);
    }

    #[test]
    fn rejects_unknown_flags() {
        let error = parse_entries(&[0x80, b'x', 0]).unwrap_err();
        assert!(error.to_string().contains("unknown"));
    }
}

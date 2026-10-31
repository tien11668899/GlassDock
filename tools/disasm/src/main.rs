//! disasm <exe> <pdb> <symbol-substring> [max-instructions] [--callers] [--list]
//! Prints the PE timestamp/size, then disassembles every public symbol whose decorated
//! name contains the substring, naming call/jump targets from the PDB. With --callers it
//! also lists the functions that contain a direct call to each match; with --list it only
//! lists the matching symbols and their RVAs.

use iced_x86::{Decoder, DecoderOptions, Formatter, Instruction, IntelFormatter};
use pdb::FallibleIterator;
use std::collections::BTreeMap;
use std::{env, fs};

struct Section {
    va: u32,
    vsize: u32,
    raw: u32,
    raw_size: u32,
    exec: bool,
}

fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn name_of(by_rva: &BTreeMap<u32, String>, rva: u32) -> String {
    match by_rva.range(..=rva).next_back() {
        Some((start, name)) if *start == rva => name.clone(),
        Some((start, name)) if rva - start < 0x4000 => format!("{}+{:#x}", name, rva - start),
        _ => format!("{:#x}", rva),
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let exe = fs::read(&args[1]).expect("read exe");
    let wanted = &args[3];
    let max: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(150);
    let callers = args.iter().any(|a| a == "--callers");

    let mut pdb = pdb::PDB::open(fs::File::open(&args[2]).expect("open pdb")).expect("parse pdb");
    let address_map = pdb.address_map().expect("address map");
    let globals = pdb.global_symbols().expect("globals");
    let mut by_rva: BTreeMap<u32, String> = BTreeMap::new();
    let mut it = globals.iter();
    while let Some(sym) = it.next().expect("symbol") {
        if let Ok(pdb::SymbolData::Public(p)) = sym.parse() {
            if let Some(rva) = p.offset.to_rva(&address_map) {
                by_rva.insert(rva.0, p.name.to_string().into_owned());
            }
        }
    }

    let pe = u32_at(&exe, 0x3c) as usize;
    let coff = pe + 4;
    let sections_count = u16_at(&exe, coff + 2) as usize;
    let timestamp = u32_at(&exe, coff + 4);
    let opt = coff + 20;
    let opt_size = u16_at(&exe, coff + 16) as usize;
    let size_of_image = u32_at(&exe, opt + 56);
    println!("timestamp={:#010x} size_of_image={:#x} publics={}", timestamp, size_of_image, by_rva.len());

    let mut sections = Vec::new();
    for i in 0..sections_count {
        let s = opt + opt_size + i * 40;
        sections.push(Section {
            va: u32_at(&exe, s + 12),
            vsize: u32_at(&exe, s + 8),
            raw: u32_at(&exe, s + 20),
            raw_size: u32_at(&exe, s + 16),
            exec: u32_at(&exe, s + 36) & 0x2000_0000 != 0,
        });
    }
    let to_off = |rva: u32| -> Option<usize> {
        sections
            .iter()
            .find(|s| rva >= s.va && rva < s.va + s.vsize.max(s.raw_size))
            .map(|s| (s.raw + (rva - s.va)) as usize)
    };

    // "imp:Name" lists every `call [__imp_Name]` site and the function it is in.
    if let Some(import) = wanted.strip_prefix("imp:") {
        let slot = by_rva
            .iter()
            .find(|(_, n)| n.as_str() == format!("__imp_{import}"))
            .map(|(r, _)| *r)
            .expect("import not found");
        for s in sections.iter().filter(|s| s.exec) {
            let start = s.raw as usize;
            let end = (s.raw + s.raw_size) as usize;
            for i in start..end.saturating_sub(6) {
                if exe[i] == 0x48 && exe[i + 1] == 0xFF && exe[i + 2] == 0x15 || exe[i] == 0xFF && exe[i + 1] == 0x15 {
                    let (len, disp_at) = if exe[i] == 0x48 { (7, i + 3) } else { (6, i + 2) };
                    let site = s.va + (i - start) as u32;
                    let disp = u32_at(&exe, disp_at) as i32;
                    if (site as i64 + len as i64 + disp as i64) as u32 == slot {
                        println!("call [{}] at {:#x} in {}", import, site, name_of(&by_rva, site));
                    }
                }
            }
        }
        return;
    }

    // "@1a2b3c" starts at that RVA instead of a symbol (to look inside a long function).
    let targets: Vec<(u32, String)> = match wanted.strip_prefix('@') {
        Some(hex) => {
            let rva = u32::from_str_radix(hex.trim_start_matches("0x"), 16).expect("hex rva");
            vec![(rva, name_of(&by_rva, rva))]
        }
        None => by_rva
            .iter()
            .filter(|(_, n)| n.contains(wanted.as_str()))
            .map(|(r, n)| (*r, n.clone()))
            .collect(),
    };

    if args.iter().any(|a| a == "--list") {
        for (rva, name) in &targets {
            println!("{:#08x}  {}", rva, name);
        }
        return;
    }

    for (rva, name) in &targets {
        println!("\n=== {} rva={:#x}", name, rva);
        let off = to_off(*rva).expect("rva not in a section");
        let bytes = &exe[off..(off + 0x800).min(exe.len())];
        println!(
            "first bytes: {}",
            bytes[..32].iter().map(|b| format!("{:02X}", b)).collect::<Vec<_>>().join(" ")
        );
        let mut decoder = Decoder::with_ip(64, bytes, *rva as u64, DecoderOptions::NONE);
        let mut formatter = IntelFormatter::new();
        let mut instr = Instruction::default();
        let mut text = String::new();
        for _ in 0..max {
            if !decoder.can_decode() {
                break;
            }
            decoder.decode_out(&mut instr);
            text.clear();
            formatter.format(&instr, &mut text);
            let mut note = String::new();
            if instr.is_call_near() || instr.is_jmp_near() || instr.is_jcc_short_or_near() {
                let t = instr.near_branch_target() as u32;
                if !(t >= *rva && t < *rva + 0x800) || instr.is_call_near() {
                    note = format!("  ; {}", name_of(&by_rva, t));
                }
            } else if instr.is_ip_rel_memory_operand() {
                note = format!("  ; [{}]", name_of(&by_rva, instr.ip_rel_memory_address() as u32));
            }
            let raw = &bytes[(instr.ip() as u32 - rva) as usize..(instr.ip() as u32 - rva) as usize + instr.len()];
            println!(
                "{:#08x}  {:<24} {}{}",
                instr.ip(),
                raw.iter().map(|b| format!("{:02X}", b)).collect::<Vec<_>>().join(""),
                text,
                note
            );
        }

        if callers {
            let mut found = BTreeMap::new();
            for s in sections.iter().filter(|s| s.exec) {
                let start = s.raw as usize;
                let end = (s.raw + s.raw_size) as usize;
                for i in start..end.saturating_sub(5) {
                    if exe[i] == 0xE8 {
                        let site = s.va + (i - start) as u32;
                        let rel = u32_at(&exe, i + 1) as i32;
                        if (site as i64 + 5 + rel as i64) as u32 == *rva {
                            found.insert(site, name_of(&by_rva, site));
                        }
                    }
                }
            }
            println!("--- direct callers ({}):", found.len());
            for (site, n) in found {
                println!("  call at {:#x} in {}", site, n);
            }
        }
    }
}

use gcrecomp_core::recompiler::analysis::FunctionMetadata;
use gcrecomp_core::recompiler::codegen::CodeGenerator;
use gcrecomp_core::recompiler::decoder::{DecodedInstruction, Instruction, InstructionType};
use std::collections::{BTreeMap, BTreeSet};
use std::{env, fs, path::Path};

fn be(b: &[u8], o: usize) -> u32 { u32::from_be_bytes([b[o], b[o+1], b[o+2], b[o+3]]) }

struct Blob { name: String, base: u32, words: Vec<u32> }

fn main() {
    let root = env::args().nth(1).expect("root");
    let out = env::args().nth(2).expect("out dir");
    let mut blobs = vec![];
    let dol = fs::read(Path::new(&root).join("sys/main.dol")).unwrap();
    for i in 0..7 {
        let (off, addr, size) = (be(&dol, i*4) as usize, be(&dol, 0x48+i*4), be(&dol, 0x90+i*4) as usize);
        if size == 0 { continue; }
        blobs.push(Blob { name: format!("main.dol text{i}"), base: addr,
            words: (0..size/4).map(|k| be(&dol, off + k*4)).collect() });
    }
    let mut rels: Vec<_> = fs::read_dir(Path::new(&root).join("files")).unwrap()
        .filter_map(|e| e.ok()).map(|e| e.path())
        .filter(|p| p.extension().map_or(false, |x| x == "rel")).collect();
    rels.sort();
    for p in rels {
        let r = fs::read(&p).unwrap();
        let (n, so) = (be(&r, 0xC) as usize, be(&r, 0x10) as usize);
        for i in 0..n {
            let (o, s) = (be(&r, so+i*8), be(&r, so+i*8+4) as usize);
            if o & 1 == 0 || s == 0 { continue; }
            let o = (o & !3) as usize;
            // REL code is unrelocated; give each module a fake base so addresses stay unique.
            blobs.push(Blob { name: format!("{} sec{i}", p.file_name().unwrap().to_string_lossy()),
                base: 0x9000_0000 + blobs.len() as u32 * 0x0100_0000,
                words: (0..s/4).map(|k| be(&r, o + k*4)).collect() });
        }
    }

    let mut total = 0usize; let mut unknown = 0usize; let mut errors = 0usize;
    let mut unk_by_op: BTreeMap<u32, usize> = BTreeMap::new();
    let mut by_type: BTreeMap<String, usize> = BTreeMap::new();
    let (mut fn_ok, mut fn_fail, mut fn_with_unknown, mut fn_total) = (0, 0, 0, 0);
    let mut fail_reasons: BTreeMap<String, usize> = BTreeMap::new();
    let mut cg = CodeGenerator::new();
    let mut sample = String::new();
    let mut per_blob = String::new();

    for b in &blobs {
        let mut dec: Vec<Option<DecodedInstruction>> = Vec::with_capacity(b.words.len());
        let (mut bt, mut bu) = (0, 0);
        for (k, &w) in b.words.iter().enumerate() {
            total += 1; bt += 1;
            let addr = b.base + (k as u32) * 4;
            match Instruction::decode(w, addr) {
                Ok(d) => {
                    *by_type.entry(format!("{:?}", d.instruction.instruction_type)).or_default() += 1;
                    if matches!(d.instruction.instruction_type, InstructionType::Unknown) && w != 0 {
                        unknown += 1; bu += 1; *unk_by_op.entry(w >> 26).or_default() += 1;
                    }
                    dec.push(Some(d));
                }
                Err(_) => { errors += 1; bu += 1; *unk_by_op.entry(w >> 26).or_default() += 1; dec.push(None); }
            }
        }
        per_blob.push_str(&format!("{:22} {:>8} instrs  {:>6.2}% not translatable\n", b.name, bt, 100.0 * bu as f64 / bt.max(1) as f64));

        // Function boundaries: section start + every `bl` target inside this blob.
        let mut starts: BTreeSet<usize> = BTreeSet::new();
        starts.insert(0);
        for (k, &w) in b.words.iter().enumerate() {
            if w >> 26 == 18 && w & 3 == 1 {
                let mut li = (w & 0x03FF_FFFC) as i64; if li & 0x0200_0000 != 0 { li -= 0x0400_0000; }
                let t = k as i64 + li / 4;
                if t >= 0 && (t as usize) < b.words.len() { starts.insert(t as usize); }
            }
        }
        let s: Vec<usize> = starts.into_iter().collect();
        for (i, &st) in s.iter().enumerate() {
            let en = s.get(i + 1).copied().unwrap_or(b.words.len());
            let insts: Vec<DecodedInstruction> = dec[st..en].iter().flatten().cloned().collect();
            fn_total += 1;
            let has_unknown = dec[st..en].iter().any(|d| d.as_ref().map_or(true,
                |d| matches!(d.instruction.instruction_type, InstructionType::Unknown) && d.raw != 0));
            if has_unknown { fn_with_unknown += 1; }
            let md = FunctionMetadata { address: b.base + st as u32 * 4, name: format!("fn"),
                size: ((en - st) * 4) as u32, calling_convention: "ppc_eabi".into(),
                parameters: vec![], return_type: None, local_variables: vec![], basic_blocks: vec![] };
            match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| cg.generate_function(&md, &insts))) {
                Ok(Ok(code)) => { fn_ok += 1; if sample.len() < 40_000 && b.name.starts_with("main") && st > 2000 && !has_unknown { sample.push_str(&code); sample.push('\n'); } }
                Ok(Err(e)) => { fn_fail += 1; let k = format!("{e}"); *fail_reasons.entry(k.chars().take(80).collect()).or_default() += 1; }
                Err(_) => { fn_fail += 1; *fail_reasons.entry("PANIC".into()).or_default() += 1; }
            }
        }
    }

    let mut r = String::new();
    r += &format!("Total instructions decoded: {total}\nUnknown (untranslatable) instructions: {unknown} ({:.2}%)\nDecode errors: {errors}\n\n", 100.0 * (unknown+errors) as f64 / total as f64);
    r += "Unknown instructions by primary opcode:\n";
    for (op, n) in &unk_by_op { r += &format!("  opcode {op:2}: {n}\n"); }
    r += "\nDecoded instruction categories:\n";
    for (t, n) in &by_type { r += &format!("  {t:18} {n}\n"); }
    r += &format!("\nFunctions (bl-target heuristic): {fn_total}\n  codegen succeeded: {fn_ok}\n  codegen failed: {fn_fail}\n  containing untranslatable instructions: {fn_with_unknown} ({:.1}%)\n", 100.0 * fn_with_unknown as f64 / fn_total as f64);
    r += "\nCodegen failure reasons:\n";
    for (k, n) in &fail_reasons { r += &format!("  {n:6}  {k}\n"); }
    r += "\nPer code section:\n"; r += &per_blob;
    fs::create_dir_all(&out).unwrap();
    fs::write(Path::new(&out).join("probe_report.txt"), &r).unwrap();
    fs::write(Path::new(&out).join("sample_generated.rs"), &sample).unwrap();
    print!("{r}");
}

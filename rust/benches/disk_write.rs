#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Rust twin of `benchmarks/cpp/tinyio_disk_bench.cpp`: sequential `O_DIRECT`
//! writes of one block (64 KiB by default), by 1, 2, 4, 8, 16 and 32 fibers on
//! one ring, each fiber writing its own non-overlapping segment. The file is
//! preallocated just before each variant (`fallocate`, outside the timing).
//!
//! One difference: the C++ twin submits `Writev` with a single iovec; this
//! submits `Write`, the same I/O through a different opcode.
//!
//!     cargo bench --bench disk_write -- [--path=P] [--bytes=256M] [--block=64K]

use std::fs::{File, OpenOptions};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::time::Instant;

use tinyio::{Io, Options, scope};

fn parse_size(s: &str) -> u64 {
    let (digits, mult) = match s.chars().last() {
        Some('k' | 'K') => (&s[..s.len() - 1], 1u64 << 10),
        Some('m' | 'M') => (&s[..s.len() - 1], 1 << 20),
        Some('g' | 'G') => (&s[..s.len() - 1], 1 << 30),
        _ => (s, 1),
    };
    digits.parse::<u64>().unwrap() * mult
}

/// `--name=value`, the C++ twin's form.
fn arg(name: &str) -> Option<String> {
    let prefix = format!("{name}=");
    std::env::args().find_map(|a| a.strip_prefix(&prefix).map(str::to_string))
}

/// Create the file and reserve its blocks (not timed).
fn prepare_file(path: &Path, bytes: u64) {
    let file = File::create(path).unwrap();
    let mut io = Io::new(Options::default()).unwrap();
    io.spawn(move |cx| cx.fallocate(&file, 0, bytes).unwrap())
        .unwrap();
    io.run_until_done();
}

fn open_direct(path: &Path) -> File {
    OpenOptions::new()
        .write(true)
        .custom_flags(libc::O_DIRECT | libc::O_CLOEXEC)
        .open(path)
        .unwrap()
}

/// A block whose start is 4096-aligned, with no `unsafe`: over-allocate and slice.
fn aligned_block(len: usize) -> (Vec<u8>, usize) {
    let v = vec![0x42u8; len + 4096];
    let off = v.as_ptr().align_offset(4096);
    (v, off)
}

fn report(name: &str, seconds: f64, bytes: u64) {
    println!(
        "  {name:<20}{seconds:.6}s  {:.3} MiB/s",
        bytes as f64 / (1024.0 * 1024.0) / seconds
    );
}

/// `fibers` fibers, each writing `segment` bytes of its own region.
fn bench(path: &Path, block: &[u8], segment: u64, fibers: usize) {
    let total = segment * fibers as u64;
    prepare_file(path, total);
    let file = open_direct(path);
    let mut io = Io::new(Options::default()).unwrap();
    // The fiber must own its block, and an O_DIRECT block must be 4096-aligned.
    let (mut owned, off) = aligned_block(block.len());
    owned[off..off + block.len()].copy_from_slice(block);
    let block_len = block.len() as u64;

    let start = Instant::now();
    io.spawn(move |cx| {
        let data = &owned[off..off + block_len as usize];
        scope(cx, |s| {
            for i in 0..fibers as u64 {
                let file = &file;
                s.spawn(move |cx| {
                    let (begin, end) = (i * segment, (i + 1) * segment);
                    let mut offset = begin;
                    while offset < end {
                        let len = block_len.min(end - offset) as usize;
                        let n = cx.write(file, &data[..len], Some(offset)).unwrap();
                        assert_eq!(n, len, "short write");
                        offset += len as u64;
                    }
                })
                .unwrap();
            }
        });
    })
    .unwrap();
    io.run_until_done();
    report(
        &format!("fibers={fibers}"),
        start.elapsed().as_secs_f64(),
        total,
    );
}

fn main() {
    let path: PathBuf =
        arg("--path").map_or_else(|| "/tmp/tinyio-rs-disk-bench.dat".into(), PathBuf::from);
    let bytes = arg("--bytes").map_or(1 << 30, |v| parse_size(&v));
    let block_len = arg("--block").map_or(64 << 10, |v| parse_size(&v)) as usize;
    assert!(
        block_len > 0 && bytes > 0 && block_len.is_multiple_of(4096),
        "block must be a multiple of 4096"
    );

    println!("Benchmarking disk write (O_DIRECT, sequential per fiber):");
    println!("  Total bytes : {bytes}\n  Block size  : {block_len}\n  Path        : {path:?}\n");
    let block = vec![0x42u8; block_len];
    for fibers in [1usize, 2, 4, 8, 16, 32] {
        let segment = (bytes / fibers as u64 / block_len as u64) * block_len as u64;
        assert!(segment > 0, "too many fibers for --bytes/--block");
        bench(&path, &block, segment, fibers);
    }
    let _ = std::fs::remove_file(&path);
}

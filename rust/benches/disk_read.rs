#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Rust twin of `benchmarks/cpp/tinyio_disk_read_bench.cpp`, same methodology:
//! 4 KiB `O_DIRECT` reads with full content verification, five scenarios
//! (file count, depth) = (1,1) (1,32) (1,128) (32,32) (128,128), depth = number
//! of fibers = queue depth, block index `(i * 977 + worker * 131) % blocks`,
//! a warm-up pass before each measured pass, and IOPS / mean / p50 / p99 as
//! medians over `--repeats` runs. The content pattern is the C++ one byte for byte.
//!
//! The data file is created in the current directory just before the run (a
//! stale file on an SSD with an SLC cache reads about twice as slow), or pass
//! `--file` to read an existing one.
//!
//!     cargo bench --bench disk_read -- [--operations N] [--repeats N] [--file-mib N]
//!                                       [--file PATH] [--scenario 0..4] [--no-verify]
//!
//! `--no-verify` skips the content check, to measure the runtime without the
//! checker's CPU cost (the C++ twin has no such switch).

use std::cell::RefCell;
use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::Instant;

use tinyio::{Io, Options, scope};

const BLOCK: usize = 4096;

/// `O_DIRECT` needs the buffer aligned; this is, with no `unsafe`.
#[repr(align(4096))]
struct Aligned([u8; BLOCK]);

fn pattern(block: u64, word: usize) -> u64 {
    (block + 1).wrapping_mul(0x9e37_79b9_7f4a_7c15)
        ^ (word as u64 + 1).wrapping_mul(0xd1b5_4a32_d192_ed03)
}

fn fill(buf: &mut [u8], first_block: u64) {
    for (b, block) in buf.chunks_exact_mut(BLOCK).enumerate() {
        for (w, word) in block.chunks_exact_mut(8).enumerate() {
            word.copy_from_slice(&pattern(first_block + b as u64, w).to_ne_bytes());
        }
    }
}

/// Blocks, then words, with `u64` compares: no per-word division, and no
/// slice comparison (which calls `bcmp` for every 8 bytes).
fn verify(buf: &[u8], first_block: u64) -> bool {
    buf.chunks_exact(BLOCK).enumerate().all(|(b, block)| {
        block.chunks_exact(8).enumerate().all(|(w, word)| {
            let got = u64::from_ne_bytes([
                word[0], word[1], word[2], word[3], word[4], word[5], word[6], word[7],
            ]);
            got == pattern(first_block + b as u64, w)
        })
    })
}

struct DataFile {
    path: PathBuf,
    blocks: usize,
    external: bool,
}

impl DataFile {
    fn new(blocks: usize, existing: Option<PathBuf>) -> DataFile {
        if let Some(path) = existing {
            return DataFile {
                path,
                blocks,
                external: true,
            };
        }
        let path = PathBuf::from(format!("tinyio-rs-disk-data-{}", std::process::id()));
        let mut out = BufWriter::with_capacity(1 << 20, File::create(&path).unwrap());
        let mut chunk = vec![0u8; BLOCK];
        for block in 0..blocks {
            fill(&mut chunk, block as u64);
            out.write_all(&chunk).unwrap();
        }
        out.into_inner().unwrap().sync_all().unwrap();
        DataFile {
            path,
            blocks,
            external: false,
        }
    }

    fn open_direct(&self) -> File {
        OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_DIRECT | libc::O_CLOEXEC)
            .open(&self.path)
            .unwrap()
    }
}

impl Drop for DataFile {
    fn drop(&mut self) {
        if !self.external {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}

struct Worker {
    buffer: Box<Aligned>,
    latencies: Vec<f64>,
    errors: usize,
    short_reads: usize,
    corrupt: usize,
}

impl Worker {
    fn new() -> Worker {
        Worker {
            buffer: Box::new(Aligned([0; BLOCK])),
            latencies: Vec::new(),
            errors: 0,
            short_reads: 0,
            corrupt: 0,
        }
    }

    fn reset(&mut self) {
        self.latencies.clear();
        self.errors = 0;
        self.short_reads = 0;
        self.corrupt = 0;
    }
}

/// One `Io` per phase, like the C++ twin.
fn run_phase(
    files: &Rc<Vec<File>>,
    workers: Vec<Worker>,
    total_ops: usize,
    blocks: usize,
    check: bool,
) -> Vec<Worker> {
    let depth = workers.len();
    let mut io = Io::new(Options::default()).unwrap();
    let done = Rc::new(RefCell::new(None));
    let (files, out) = (files.clone(), done.clone());
    io.spawn(move |cx| {
        let mut workers = workers;
        scope(cx, |s| {
            for (index, worker) in workers.iter_mut().enumerate() {
                let count = total_ops / depth + usize::from(index < total_ops % depth);
                let file = &files[index % files.len()];
                s.spawn(move |cx| {
                    for iteration in 0..count {
                        let block = (iteration * 977 + index * 131) % blocks;
                        let start = Instant::now();
                        let result =
                            cx.read(file, &mut worker.buffer.0, Some((block * BLOCK) as u64));
                        worker.latencies.push(start.elapsed().as_secs_f64() * 1e6);
                        match result {
                            Err(_) => return worker.errors += 1,
                            Ok(n) if n != BLOCK => return worker.short_reads += 1,
                            Ok(_) => {}
                        }
                        if check && !verify(&worker.buffer.0, block as u64) {
                            worker.corrupt += 1;
                        }
                    }
                })
                .unwrap();
            }
        });
        *out.borrow_mut() = Some(workers);
    })
    .unwrap();
    io.run_until_done();
    done.borrow_mut().take().unwrap()
}

struct Measurement {
    iops: f64,
    mean: f64,
    p50: f64,
    p99: f64,
    errors: usize,
    short_reads: usize,
    corrupt: usize,
}

fn measure(
    operations: usize,
    data: &DataFile,
    file_count: usize,
    depth: usize,
    check: bool,
) -> Measurement {
    let files: Rc<Vec<File>> = Rc::new((0..file_count).map(|_| data.open_direct()).collect());
    let mut workers: Vec<Worker> = (0..depth).map(|_| Worker::new()).collect();

    workers = run_phase(&files, workers, depth * 8, data.blocks, check); // warm-up
    for w in &mut workers {
        assert!(
            w.errors + w.short_reads + w.corrupt == 0,
            "warmup verification failed"
        );
        w.reset();
    }

    let start = Instant::now();
    let workers = run_phase(&files, workers, operations, data.blocks, check);
    let seconds = start.elapsed().as_secs_f64();

    let mut m = Measurement {
        iops: 0.0,
        mean: 0.0,
        p50: 0.0,
        p99: 0.0,
        errors: 0,
        short_reads: 0,
        corrupt: 0,
    };
    let mut latencies = Vec::with_capacity(operations);
    for w in &workers {
        latencies.extend_from_slice(&w.latencies);
        m.errors += w.errors;
        m.short_reads += w.short_reads;
        m.corrupt += w.corrupt;
    }
    assert_eq!(latencies.len(), operations, "incorrect measurement count");
    latencies.sort_by(f64::total_cmp);
    m.iops = operations as f64 / seconds;
    m.mean = latencies.iter().sum::<f64>() / latencies.len() as f64;
    m.p50 = latencies[(latencies.len() - 1) / 2];
    m.p99 = latencies[(0.99 * (latencies.len() - 1) as f64) as usize];
    m
}

fn median(samples: &[Measurement], f: impl Fn(&Measurement) -> f64) -> f64 {
    let mut v: Vec<f64> = samples.iter().map(f).collect();
    v.sort_by(f64::total_cmp);
    (v[(v.len() - 1) / 2] + v[v.len() / 2]) / 2.0
}

fn arg(name: &str) -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter()
        .position(|a| a == name)
        .and_then(|i| args.get(i + 1))
        .cloned()
}

fn main() {
    let operations: usize = arg("--operations").map_or(20_000, |v| v.parse().unwrap());
    let repeats: usize = arg("--repeats").map_or(3, |v| v.parse().unwrap());
    let file_mib: usize = arg("--file-mib").map_or(64, |v| v.parse().unwrap());
    let scenario: Option<usize> = arg("--scenario").map(|v| v.parse().unwrap());
    let check = !std::env::args().any(|a| a == "--no-verify");
    assert!((128..=2_000_000).contains(&operations) && (1..=20).contains(&repeats));

    let data = DataFile::new(
        file_mib * 1024 * 1024 / BLOCK,
        arg("--file").map(PathBuf::from),
    );
    println!(
        "tinyio-rs disk bench, operations/run={operations}, repeats={repeats}, file={file_mib} MiB, cwd={}",
        std::env::current_dir().unwrap().display()
    );
    println!("IOPS/latency: per-run medians; errors/short/corrupt: totals across runs.");
    println!(
        "{:<12} {:<12} {:>4} {:>12} {:>12} {:>12} {:>12} {:>8} {:>8} {:>8}",
        "Scenario",
        "Backend",
        "QD",
        "IOPS",
        "Mean(us)",
        "P50(us)",
        "P99(us)",
        "Errors",
        "Short",
        "Corrupt"
    );
    let mut passed = true;
    for (index, (file_count, depth)) in [(1, 1), (1, 32), (1, 128), (32, 32), (128, 128)]
        .into_iter()
        .enumerate()
    {
        if scenario.is_some_and(|s| s != index) {
            continue;
        }
        let samples: Vec<Measurement> = (0..repeats)
            .map(|_| measure(operations, &data, file_count, depth, check))
            .collect();
        let (e, s, c) = samples.iter().fold((0, 0, 0), |a, m| {
            (a.0 + m.errors, a.1 + m.short_reads, a.2 + m.corrupt)
        });
        println!(
            "{:<12} {:<12} {:>4} {:>12.0} {:>12.2} {:>12.2} {:>12.2} {:>8} {:>8} {:>8}",
            if file_count == 1 {
                "shared"
            } else {
                "independent"
            },
            "tinyio-rs",
            depth,
            median(&samples, |m| m.iops),
            median(&samples, |m| m.mean),
            median(&samples, |m| m.p50),
            median(&samples, |m| m.p99),
            e,
            s,
            c
        );
        passed &= e + s + c == 0;
    }
    drop(data); // `exit` skips destructors, and the data file is removed in `drop`
    std::process::exit(i32::from(!passed));
}

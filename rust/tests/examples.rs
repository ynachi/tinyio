#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! The examples, run as real processes (`cargo test` builds them). Each test
//! states its prediction first.

mod common;

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use common::TmpDir;

fn example(name: &str) -> Command {
    let mut path = std::env::current_exe().unwrap();
    path.pop(); // .../deps
    path.pop();
    let path: PathBuf = path.join("examples").join(name);
    assert!(path.exists(), "{path:?} is not built");
    Command::new(path)
}

fn free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

/// Kills the child when the test ends, whatever happens.
struct Server(Child);

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn wait_until_listening(port: u16) {
    let addr = SocketAddr::from(([127, 0, 0, 1], port));
    let deadline = Instant::now() + Duration::from_secs(10);
    while TcpStream::connect(addr).is_err() {
        assert!(Instant::now() < deadline, "server never listened on {port}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

// Prediction: example 01 echoes for example 04, which checks every message and
// exits 0 with no errors.
#[test]
fn echo_server_and_client_examples() {
    let port = free_port();
    let _server = Server(
        example("01_hello_echo")
            .arg(format!("--port={port}"))
            .arg("--ip=127.0.0.1")
            .stdout(Stdio::null())
            .spawn()
            .unwrap(),
    );
    wait_until_listening(port);
    let out = example("04_echo_client")
        .args([
            format!("--port={port}"),
            "--threads=3".into(),
            "--clients-per-thread=5".into(),
        ])
        .args(["--messages=40".to_string()])
        .output()
        .unwrap();
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success(),
        "{text}\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(text.contains("total success: 15"), "{text}");
    assert!(text.contains("total errors: 0"), "{text}");
}

// Prediction: example 01 shuts itself down after its accept timeout with no client.
#[test]
fn echo_server_ends_after_the_accept_timeout() {
    let port = free_port();
    let mut child = example("01_hello_echo")
        .args([
            format!("--port={port}"),
            "--ip=127.0.0.1".into(),
            "--accept-timeout-secs=1".into(),
        ])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        if let Some(status) = child.try_wait().unwrap() {
            assert!(status.success(), "{status}");
            return;
        }
        assert!(Instant::now() < deadline, "the server did not stop");
        std::thread::sleep(Duration::from_millis(50));
    }
}

// Prediction: example 03 answers an HTTP request on a keep-alive connection
// twice, and stops by itself after --seconds.
#[test]
fn http_server_example_answers_and_stops() {
    let port = free_port();
    let mut child = example("03_http_services")
        .args([
            format!("--port={port}"),
            "--workers=2".into(),
            "--seconds=3".into(),
        ])
        .stdout(Stdio::null())
        .spawn()
        .unwrap();
    wait_until_listening(port);
    let mut conn = TcpStream::connect(("127.0.0.1", port)).unwrap();
    conn.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    for _ in 0..2 {
        conn.write_all(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            .unwrap();
        let mut buf = [0u8; 256];
        let n = conn.read(&mut buf).unwrap();
        let text = String::from_utf8_lossy(&buf[..n]);
        assert!(text.starts_with("HTTP/1.1 200 OK"), "{text}");
        assert!(text.ends_with("Hello, io_uring!"), "{text}");
    }
    drop(conn);
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        if let Some(status) = child.try_wait().unwrap() {
            assert!(status.success(), "{status}");
            return;
        }
        assert!(Instant::now() < deadline, "the server ignored --seconds");
        std::thread::sleep(Duration::from_millis(50));
    }
}

// Prediction: example 02 probes an open port UP and a closed one DOWN (refused).
#[test]
fn fanout_example_reports_up_and_down() {
    let open = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = open.local_addr().unwrap().port();
    let out = example("02_fanout")
        .arg(format!("--port={port}"))
        .arg("127.0.0.1")
        .output()
        .unwrap();
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(out.status.success(), "{text}");
    assert!(text.contains("127.0.0.1") && text.contains("UP"), "{text}");

    drop(open); // now nothing listens there
    let out = example("02_fanout")
        .arg(format!("--port={port}"))
        .arg("127.0.0.1")
        .output()
        .unwrap();
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(text.contains("DOWN  (refused)"), "{text}");
}

// Prediction: example 05 writes a data file of the asked size, then reads it back at random.
#[test]
fn disk_bench_example_writes_then_reads() {
    let dir = TmpDir::new("bench-io");
    let file = dir.join("bench.dat");
    let out = example("05_bench_io")
        .args([
            "write".to_string(),
            format!("--file={}", file.display()),
            "--mb=8".into(),
        ])
        .output()
        .unwrap();
    assert!(out.status.success());
    assert!(String::from_utf8_lossy(&out.stdout).contains("Sequential write (8 MiB)"));
    assert_eq!(std::fs::metadata(&file).unwrap().len(), 8 << 20);

    let out = example("05_bench_io")
        .args([
            "read".to_string(),
            format!("--file={}", file.display()),
            "--mb=8".into(),
        ])
        .args(["--reads=2000".to_string()])
        .output()
        .unwrap();
    assert!(out.status.success());
    assert!(String::from_utf8_lossy(&out.stdout).contains("Random read (2000 x 4 KiB)"));
}

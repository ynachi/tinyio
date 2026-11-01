#![forbid(unsafe_code)]
#![allow(clippy::unwrap_used, clippy::expect_used, clippy::panic)]
//! Layer 6: TCP on loopback. Each test states its prediction first.

mod common;

use std::cell::{Cell, RefCell};
use std::io::{Read, Write};
use std::net::{Shutdown, SocketAddr};
use std::rc::Rc;
use std::time::{Duration, Instant};

use common::watchdog;
use tinyio::net::{PollEvents, TcpListener, TcpStream};
use tinyio::{Io, Options, is_cancelled};

const MS: Duration = Duration::from_millis(1);

fn io() -> Io {
    watchdog(60);
    Io::new(Options::default()).unwrap()
}

fn loopback() -> SocketAddr {
    "127.0.0.1:0".parse().unwrap()
}

/// A port nothing listens on.
fn closed_port() -> SocketAddr {
    let l = std::net::TcpListener::bind(loopback()).unwrap();
    l.local_addr().unwrap()
}

// Prediction (the stage's echo test): 20 clients each send 50 distinct messages
// to an accept loop that spawns an echo fiber per connection, all in one Io,
// and every message comes back intact. The last client stops the loop, which
// cancels the accept.
#[test]
fn echo_many_clients_many_messages() {
    const CLIENTS: usize = 20;
    const MESSAGES: usize = 50;
    let mut io = io();
    let listener = Rc::new(TcpListener::bind(loopback()).unwrap());
    let addr = listener.local_addr().unwrap();
    let remote = io.remote();
    let ok = Rc::new(Cell::new(0));
    let accepted = Rc::new(Cell::new(0));

    let (l, acc) = (listener.clone(), accepted.clone());
    io.spawn(move |cx| {
        loop {
            let fd = match cx.accept(&*l) {
                Ok(fd) => fd,
                Err(e) if is_cancelled(&e) => return,
                Err(e) => panic!("accept: {e}"),
            };
            acc.set(acc.get() + 1);
            cx.spawn(move |cx| {
                let conn = TcpStream::from(fd);
                let mut buf = [0u8; 256];
                loop {
                    let n = cx.read(&conn, &mut buf, None).unwrap();
                    if n == 0 {
                        return;
                    }
                    let mut sent = 0;
                    while sent < n {
                        sent += cx.write(&conn, &buf[sent..n], None).unwrap();
                    }
                }
            })
            .unwrap();
        }
    })
    .unwrap();

    for c in 0..CLIENTS {
        let (ok, remote) = (ok.clone(), remote.clone());
        io.spawn(move |cx| {
            let conn = TcpStream::connect(cx, addr).unwrap();
            for m in 0..MESSAGES {
                let msg = format!("client {c} message {m}");
                let mut sent = 0;
                while sent < msg.len() {
                    sent += cx.write(&conn, &msg.as_bytes()[sent..], None).unwrap();
                }
                let mut got = vec![0u8; msg.len()];
                let mut have = 0;
                while have < got.len() {
                    let n = cx.read(&conn, &mut got[have..], None).unwrap();
                    assert!(n > 0, "server closed early");
                    have += n;
                }
                assert_eq!(got, msg.as_bytes());
            }
            ok.set(ok.get() + 1);
            if ok.get() == CLIENTS {
                remote.stop();
            }
        })
        .unwrap();
    }
    io.run();
    assert_eq!(ok.get(), CLIENTS);
    assert_eq!(accepted.get(), CLIENTS);
}

// Prediction: the same echo through std::io::Read/Write on `stream.with(cx)`:
// the server is a single `io::copy`, the client uses write_all and read_exact.
#[test]
fn std_io_adapter_echo() {
    let mut io = io();
    let listener = Rc::new(TcpListener::bind(loopback()).unwrap());
    let addr = listener.local_addr().unwrap();
    let l = listener.clone();
    io.spawn(move |cx| {
        let conn = TcpStream::from(cx.accept(&*l).unwrap());
        let (mut r, mut w) = (conn.with(cx), conn.with(cx));
        let copied = std::io::copy(&mut r, &mut w).unwrap(); // until the client closes
        assert_eq!(copied, 5000);
    })
    .unwrap();
    let remote = io.remote();
    io.spawn(move |cx| {
        let conn = TcpStream::connect(cx, addr).unwrap();
        let payload: Vec<u8> = (0..5000u32).map(|i| (i % 251) as u8).collect();
        let mut io = conn.with(cx);
        io.write_all(&payload).unwrap();
        let mut back = vec![0u8; payload.len()];
        io.read_exact(&mut back).unwrap();
        assert_eq!(back, payload);
        conn.shutdown(Shutdown::Write).unwrap();
        let mut rest = Vec::new();
        io.read_to_end(&mut rest).unwrap(); // EOF once the server's copy ends
        assert!(rest.is_empty());
        remote.stop();
    })
    .unwrap();
    io.run();
}

// Prediction: accept_addr reports the peer the client connected from.
#[test]
fn accept_addr_is_the_clients_address() {
    let mut io = io();
    let listener = Rc::new(TcpListener::bind(loopback()).unwrap());
    let addr = listener.local_addr().unwrap();
    let seen = Rc::new(RefCell::new(None));
    let l = listener.clone();
    let s = seen.clone();
    io.spawn(move |cx| {
        let (fd, peer) = cx.accept_addr(&*l).unwrap();
        *s.borrow_mut() = Some(peer);
        drop(fd);
    })
    .unwrap();
    let local = Rc::new(RefCell::new(None));
    let lo = local.clone();
    io.spawn(move |cx| {
        let c = TcpStream::connect(cx, addr).unwrap();
        *lo.borrow_mut() = Some(c.local_addr().unwrap());
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*seen.borrow(), *local.borrow());
    assert!(seen.borrow().is_some());
}

// Prediction: connecting to a port nobody listens on is ConnectionRefused.
#[test]
fn connect_refused() {
    let mut io = io();
    let addr = closed_port();
    let kind = Rc::new(Cell::new(None));
    let k = kind.clone();
    io.spawn(move |cx| k.set(Some(TcpStream::connect(cx, addr).unwrap_err().kind())))
        .unwrap();
    io.run_until_done();
    assert_eq!(kind.get(), Some(std::io::ErrorKind::ConnectionRefused));
}

// Prediction: accept with a deadline and no client fails with TimedOut after it.
#[test]
fn accept_times_out() {
    let mut io = io();
    let listener = TcpListener::bind(loopback()).unwrap();
    let took = Rc::new(Cell::new(Duration::ZERO));
    let t = took.clone();
    io.spawn(move |cx| {
        let start = Instant::now();
        let e = cx.timeout(100 * MS).accept(&listener).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::TimedOut);
        t.set(start.elapsed());
    })
    .unwrap();
    io.run_until_done();
    assert!(
        took.get() >= 100 * MS && took.get() < 400 * MS,
        "{:?}",
        took.get()
    );
}

// Prediction: poll reports OUT at once on a fresh connection, and IN only after
// the peer has written; a poll with a deadline and nothing to report times out.
#[test]
fn poll_reports_readiness() {
    let mut io = io();
    let listener = Rc::new(TcpListener::bind(loopback()).unwrap());
    let addr = listener.local_addr().unwrap();
    let l = listener.clone();
    let server_conn = Rc::new(RefCell::new(None));
    let sc = server_conn.clone();
    io.spawn(move |cx| *sc.borrow_mut() = Some(TcpStream::from(cx.accept(&*l).unwrap())))
        .unwrap();
    let log = Rc::new(RefCell::new(Vec::new()));
    let lg = log.clone();
    let sc = server_conn.clone();
    io.spawn(move |cx| {
        let c = TcpStream::connect(cx, addr).unwrap();
        let ev = cx.poll(&c, PollEvents::OUT).unwrap();
        lg.borrow_mut()
            .push(format!("out={}", ev.contains(PollEvents::OUT)));
        let e = cx.timeout(60 * MS).poll(&c, PollEvents::IN).unwrap_err();
        lg.borrow_mut().push(format!("quiet={:?}", e.kind()));
        while sc.borrow().is_none() {
            cx.yield_now().unwrap();
        }
        let peer = sc.borrow_mut().take().unwrap();
        cx.write(&peer, b"x", None).unwrap();
        let ev = cx.poll(&c, PollEvents::IN).unwrap();
        lg.borrow_mut()
            .push(format!("in={}", ev.contains(PollEvents::IN)));
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(*log.borrow(), ["out=true", "quiet=TimedOut", "in=true"]);
}

// Prediction: a plain bind of an address already in use fails with AddrInUse
// (like std); bind_reuse_port lets listeners share one port. The stream
// options and shutdown behave: after shutdown(Write) the peer reads EOF.
#[test]
fn reuseport_options_and_shutdown() {
    let a = TcpListener::bind_reuse_port(loopback()).unwrap();
    let port = a.local_addr().unwrap();
    let e = TcpListener::bind(port).unwrap_err();
    assert_eq!(e.kind(), std::io::ErrorKind::AddrInUse, "{e:?}");
    TcpListener::bind_reuse_port(port).expect("a second reuse-port listener on the same port");

    let mut io = io();
    let a = Rc::new(a);
    let l = a.clone();
    let got = Rc::new(Cell::new(usize::MAX));
    let g = got.clone();
    io.spawn(move |cx| {
        let s = TcpStream::from(cx.accept(&*l).unwrap());
        s.set_nodelay(true).unwrap();
        s.set_keepalive(true).unwrap();
        g.set(cx.read(&s, &mut [0u8; 8], None).unwrap());
    })
    .unwrap();
    io.spawn(move |cx| {
        let c = TcpStream::connect(cx, port).unwrap();
        assert_eq!(c.peer_addr().unwrap(), port);
        c.shutdown(Shutdown::Write).unwrap();
        cx.sleep(30 * MS).unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(got.get(), 0, "the peer should have read EOF");
}

// Prediction: if the machine has IPv6 loopback, an IPv6 echo round trip works too.
#[test]
fn ipv6_round_trip() {
    let listener = match TcpListener::bind("[::1]:0".parse().unwrap()) {
        Ok(l) => l,
        // Only "this machine has no IPv6" skips; any other failure is a bug.
        Err(e)
            if e.raw_os_error() == Some(libc::EAFNOSUPPORT)
                || e.kind() == std::io::ErrorKind::AddrNotAvailable =>
        {
            eprintln!("skipped: no IPv6 loopback ({e})");
            return;
        }
        Err(e) => panic!("IPv6 bind failed unexpectedly: {e}"),
    };
    let mut io = io();
    let addr = listener.local_addr().unwrap();
    assert!(addr.is_ipv6());
    let listener = Rc::new(listener);
    let got = Rc::new(Cell::new(0));
    let (l, g) = (listener.clone(), got.clone());
    io.spawn(move |cx| {
        let s = TcpStream::from(cx.accept(&*l).unwrap());
        g.set(cx.read(&s, &mut [0u8; 8], None).unwrap());
    })
    .unwrap();
    io.spawn(move |cx| {
        let c = TcpStream::connect(cx, addr).unwrap();
        cx.write(&c, b"hi", None).unwrap();
    })
    .unwrap();
    io.run_until_done();
    assert_eq!(got.get(), 2);
}

// Prediction: connect_timeout to a live listener succeeds well inside its limit,
// and `Timed::accept_addr` on a listener with no client fails with TimedOut.
#[test]
fn connect_timeout_and_timed_accept_addr() {
    let mut io = io();
    let listener = Rc::new(TcpListener::bind(loopback()).unwrap());
    let addr = listener.local_addr().unwrap();
    let log = Rc::new(RefCell::new(Vec::new()));
    let (l, lg) = (listener.clone(), log.clone());
    io.spawn(move |cx| {
        let quiet = cx.timeout(30 * MS).accept_addr(&*l).map(|_| ());
        lg.borrow_mut()
            .push(format!("quiet={:?}", quiet.map_err(|e| e.kind())));
        let (_fd, peer) = cx.timeout(2_000 * MS).accept_addr(&*l).unwrap();
        lg.borrow_mut()
            .push(format!("peer_is_loopback={}", peer.ip().is_loopback()));
    })
    .unwrap();
    let lg = log.clone();
    io.spawn(move |cx| {
        cx.sleep(100 * MS).unwrap(); // after the quiet accept has timed out
        let started = Instant::now();
        let c = TcpStream::connect_timeout(cx, addr, 2_000 * MS).unwrap();
        assert_eq!(c.peer_addr().unwrap(), addr);
        lg.borrow_mut()
            .push(format!("fast={}", started.elapsed() < 500 * MS));
        cx.sleep(50 * MS).unwrap();
    })
    .unwrap();
    io.run_until_done();
    let mut got = log.borrow().clone();
    got.sort();
    assert_eq!(
        got,
        ["fast=true", "peer_is_loopback=true", "quiet=Err(TimedOut)"]
    );
}

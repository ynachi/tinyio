#![forbid(unsafe_code)]
//! The kernel floor. tinyio needs Linux 6.9: `IORING_OP_FTRUNCATE` is the
//! newest opcode it submits. There is no fallback for an older kernel, and
//! nothing branches on the kernel version; instead `Io::new` asks the kernel
//! (`io_uring_register_probe`) whether it knows every opcode on [`REQUIRED`]
//! and refuses to build a runtime if it does not. A probe, not `uname`:
//! distribution kernels backport features, and the probe answers the question
//! that matters.

use std::io;

use io_uring::opcode;

/// The oldest kernel that knows every opcode below, as the message says it.
pub(crate) const MIN_KERNEL: &str = "6.9";

/// Every opcode the crate submits, with its name for the error message. A new
/// operation adds its opcode here; `Ring::push` checks (in debug builds, so in
/// the tests) that each SQE it is given is on this list, so forgetting fails a
/// test, not production.
pub(crate) const REQUIRED: &[(u8, &str)] = &[
    (opcode::Read::CODE, "Read"),
    (opcode::Write::CODE, "Write"),
    (opcode::Readv::CODE, "Readv"),
    (opcode::Writev::CODE, "Writev"),
    (opcode::Fsync::CODE, "Fsync"),
    (opcode::Ftruncate::CODE, "Ftruncate"),
    (opcode::Fallocate::CODE, "Fallocate"),
    (opcode::OpenAt::CODE, "OpenAt"),
    (opcode::Close::CODE, "Close"),
    (opcode::RenameAt::CODE, "RenameAt"),
    (opcode::UnlinkAt::CODE, "UnlinkAt"),
    (opcode::Statx::CODE, "Statx"),
    (opcode::Accept::CODE, "Accept"),
    (opcode::Connect::CODE, "Connect"),
    (opcode::Send::CODE, "Send"),
    (opcode::PollAdd::CODE, "PollAdd"),
    (opcode::Timeout::CODE, "Timeout"),
    (opcode::AsyncCancel::CODE, "AsyncCancel"),
];

/// Is `opcode` one the crate submits?
pub(crate) fn is_required(opcode: u32) -> bool {
    REQUIRED.iter().any(|&(code, _)| u32::from(code) == opcode)
}

/// `Ok` if `supported` says yes to every required opcode; otherwise an
/// `Unsupported` error naming the first one that is missing and the floor.
/// Pure over the answers, so a test can hand it any kernel.
pub(crate) fn check_supported(supported: impl Fn(u8) -> bool) -> io::Result<()> {
    for &(code, name) in REQUIRED {
        if !supported(code) {
            return Err(unsupported(format!(
                "this kernel's io_uring does not support the {name} opcode; \
                 tinyio requires Linux {MIN_KERNEL} or later"
            )));
        }
    }
    Ok(())
}

/// The error for a kernel too old to create the ring at all (the setup flags
/// are from 6.1 and later, so an older one answers `EINVAL` or `ENOSYS`).
pub(crate) fn setup_failed(e: io::Error) -> io::Error {
    match e.raw_os_error() {
        Some(libc::EINVAL | libc::ENOSYS) => unsupported(format!(
            "io_uring setup failed ({e}); tinyio requires Linux {MIN_KERNEL} or later"
        )),
        _ => e,
    }
}

fn unsupported(msg: String) -> io::Error {
    io::Error::new(io::ErrorKind::Unsupported, msg)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Prediction: a kernel that knows every required opcode passes.
    #[test]
    fn a_kernel_with_everything_passes() {
        assert!(check_supported(|_| true).is_ok());
    }

    // Prediction: a kernel without Ftruncate (anything before 6.9) is refused
    // with Unsupported, and the message names the opcode and the floor. The
    // answer comes from the set handed in, never from the box running the test.
    #[test]
    fn a_kernel_without_ftruncate_is_refused() {
        let e = check_supported(|op| op != opcode::Ftruncate::CODE).unwrap_err();
        assert_eq!(e.kind(), io::ErrorKind::Unsupported);
        let msg = e.to_string();
        assert!(msg.contains("Ftruncate"), "{msg}");
        assert!(msg.contains("6.9 or later"), "{msg}");
    }

    // Prediction: every required opcode, removed alone, is caught and named.
    #[test]
    fn each_required_opcode_is_checked() {
        for &(code, name) in REQUIRED {
            let e = check_supported(|op| op != code).unwrap_err();
            assert!(e.to_string().contains(name), "{name}: {e}");
        }
    }

    // Prediction: the list has no duplicates (the checks above would hide one).
    #[test]
    fn the_list_has_no_duplicates() {
        let mut codes: Vec<u8> = REQUIRED.iter().map(|&(c, _)| c).collect();
        codes.sort_unstable();
        codes.dedup();
        assert_eq!(codes.len(), REQUIRED.len());
    }

    // Prediction: this box's kernel (6.9 or later is the floor the tests run
    // on) passes the real probe; the unit tests above never ask it.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn this_box_passes_the_real_probe() {
        let ring = io_uring::IoUring::new(2).unwrap();
        let mut probe = io_uring::Probe::new();
        ring.submitter().register_probe(&mut probe).unwrap();
        check_supported(|op| probe.is_supported(op)).unwrap();
    }

    // Prediction: only EINVAL and ENOSYS from ring creation mean "kernel too
    // old"; anything else (EPERM from a sysctl, ENOMEM) passes through.
    #[test]
    fn setup_errors_map_only_when_the_kernel_is_old() {
        for code in [libc::EINVAL, libc::ENOSYS] {
            let e = setup_failed(io::Error::from_raw_os_error(code));
            assert_eq!(e.kind(), io::ErrorKind::Unsupported);
            assert!(e.to_string().contains("6.9 or later"));
        }
        let e = setup_failed(io::Error::from_raw_os_error(libc::EPERM));
        assert_eq!(e.raw_os_error(), Some(libc::EPERM));
    }
}

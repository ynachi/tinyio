#![forbid(unsafe_code)]
//! `SocketAddr` to and from the kernel's `sockaddr` bytes, in safe code: the
//! kernel only ever sees a byte buffer and a length.

use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV4, SocketAddrV6};

/// Room for any `sockaddr` (`sockaddr_storage` is 128 bytes).
#[repr(C, align(8))]
pub(crate) struct RawAddr {
    pub(crate) bytes: [u8; 128],
}

impl RawAddr {
    pub(crate) fn zeroed() -> RawAddr {
        RawAddr { bytes: [0; 128] }
    }
}

const AF_INET: u16 = libc::AF_INET as u16;
const AF_INET6: u16 = libc::AF_INET6 as u16;

/// The bytes of `addr` as a `sockaddr_in` or `sockaddr_in6`, and their length.
pub(crate) fn encode(addr: &SocketAddr) -> (RawAddr, u32) {
    let mut raw = RawAddr::zeroed();
    let b = &mut raw.bytes;
    match addr {
        SocketAddr::V4(a) => {
            b[0..2].copy_from_slice(&AF_INET.to_ne_bytes());
            b[2..4].copy_from_slice(&a.port().to_be_bytes());
            b[4..8].copy_from_slice(&a.ip().octets());
            (raw, 16)
        }
        SocketAddr::V6(a) => {
            b[0..2].copy_from_slice(&AF_INET6.to_ne_bytes());
            b[2..4].copy_from_slice(&a.port().to_be_bytes());
            b[4..8].copy_from_slice(&a.flowinfo().to_ne_bytes());
            b[8..24].copy_from_slice(&a.ip().octets());
            b[24..28].copy_from_slice(&a.scope_id().to_ne_bytes());
            (raw, 28)
        }
    }
}

/// The inverse of [`encode`]; `None` for another family or a short buffer.
pub(crate) fn decode(raw: &RawAddr, len: u32) -> Option<SocketAddr> {
    let b = &raw.bytes;
    let len = len as usize;
    match u16::from_ne_bytes([b[0], b[1]]) {
        AF_INET if len >= 16 => {
            let port = u16::from_be_bytes([b[2], b[3]]);
            let ip = Ipv4Addr::new(b[4], b[5], b[6], b[7]);
            Some(SocketAddr::V4(SocketAddrV4::new(ip, port)))
        }
        AF_INET6 if len >= 28 => {
            let port = u16::from_be_bytes([b[2], b[3]]);
            let flow = u32::from_ne_bytes([b[4], b[5], b[6], b[7]]);
            let mut octets = [0u8; 16];
            octets.copy_from_slice(&b[8..24]);
            let scope = u32::from_ne_bytes([b[24], b[25], b[26], b[27]]);
            let ip = Ipv6Addr::from(octets);
            Some(SocketAddr::V6(SocketAddrV6::new(ip, port, flow, scope)))
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Prediction: encode then decode gives back the same address, v4 and v6.
    #[test]
    fn round_trips() {
        for text in ["127.0.0.1:8080", "0.0.0.0:0", "[::1]:443"] {
            let addr: SocketAddr = text.parse().unwrap();
            let (raw, len) = encode(&addr);
            assert_eq!(decode(&raw, len), Some(addr), "{text}");
        }
        let v6 = SocketAddr::V6(SocketAddrV6::new(Ipv6Addr::LOCALHOST, 7, 0xabcd, 3));
        let (raw, len) = encode(&v6);
        assert_eq!(decode(&raw, len), Some(v6));
    }

    // Prediction: the bytes match the C layout: family (native endian), port
    // (big endian), then the address.
    #[test]
    fn matches_the_c_layout() {
        let (raw, len) = encode(&"1.2.3.4:258".parse().unwrap());
        assert_eq!(len, 16);
        assert_eq!(&raw.bytes[0..2], &AF_INET.to_ne_bytes());
        assert_eq!(&raw.bytes[2..8], &[1, 2, 1, 2, 3, 4]); // port 258 = 0x0102
    }

    // Prediction: the v6 flow label and scope id are stored raw (native
    // endian), exactly as std hands them over; only the port is big endian.
    #[test]
    fn v6_flowinfo_and_scope_are_raw() {
        let v6 = SocketAddr::V6(SocketAddrV6::new(
            Ipv6Addr::LOCALHOST,
            0x0102,
            0x0a0b_0c0d,
            7,
        ));
        let (raw, len) = encode(&v6);
        assert_eq!(len, 28);
        assert_eq!(&raw.bytes[2..4], &[1, 2]);
        assert_eq!(&raw.bytes[4..8], &0x0a0b_0c0du32.to_ne_bytes());
        assert_eq!(&raw.bytes[24..28], &7u32.to_ne_bytes());
    }

    // Prediction: a short length or an unknown family decodes to None.
    #[test]
    fn rejects_short_and_foreign() {
        let (raw, _) = encode(&"1.2.3.4:5".parse().unwrap());
        assert_eq!(decode(&raw, 8), None);
        assert_eq!(decode(&RawAddr::zeroed(), 128), None);
    }
}

//! Immutable, explicitly little-endian IPC frame response. No retained cache.
//! See bridge-contract.json/frame_packet; independent of science/ABI versions.
//!
//! Frames arrive from either bridge (the backend bridge's `BridgeFrame` or the
//! review bridge's `ReviewFrame`, ADR 0008); both convert into the plain
//! [`Frame`] this encoder takes, so the wire format has one owner.

include!("frame_packet_contract.rs");

/// Bridge-neutral frame: metadata plus one owned copy of the pixels.
#[derive(Debug, Clone, Default)]
pub struct Frame {
    pub valid: bool,
    pub frame_index: u64,
    pub timestamp_ns: u64,
    pub width: u64,
    pub height: u64,
    pub pixel_format: u64,
    pub stride_bytes: u64,
    pub data: Vec<u8>,
}

#[cfg(not(feature = "review-only"))]
impl From<mib_bridge::ffi::BridgeFrame> for Frame {
    fn from(f: mib_bridge::ffi::BridgeFrame) -> Self {
        Frame {
            valid: f.valid,
            frame_index: f.frame_index,
            timestamp_ns: f.timestamp_ns,
            width: f.width,
            height: f.height,
            pixel_format: f.pixel_format,
            stride_bytes: f.stride_bytes,
            data: f.data,
        }
    }
}

impl From<mib_bridge::review_ffi::ReviewFrame> for Frame {
    fn from(f: mib_bridge::review_ffi::ReviewFrame) -> Self {
        Frame {
            valid: f.valid,
            frame_index: f.frame_index,
            timestamp_ns: 0,
            width: f.width,
            height: f.height,
            pixel_format: f.pixel_format,
            stride_bytes: f.stride_bytes,
            data: f.data,
        }
    }
}

/// Bytes per pixel of a contract pixel format, or None when unknown.
fn bytes_per_pixel(pixel_format: u64) -> Option<u64> {
    match pixel_format {
        PIXEL_FORMAT_MONO8_LEGACY | PIXEL_FORMAT_MONO8 => Some(1),
        PIXEL_FORMAT_RGB8 => Some(3),
        _ => None,
    }
}

pub fn encode(frame: impl Into<Frame>, pull_kind: u32) -> Result<Vec<u8>, String> {
    let frame: Frame = frame.into();
    if !(1..=MAX_PULL_KIND).contains(&pull_kind) {
        return Err("FRAME_PACKET_INVALID_SOURCE".into());
    }
    if frame.valid {
        let bpp = bytes_per_pixel(frame.pixel_format);
        if frame.width == 0 || frame.height == 0
            || frame.width > MAX_DIMENSION || frame.height > MAX_DIMENSION
            || frame.width.checked_mul(frame.height).filter(|n| *n <= MAX_PIXELS).is_none()
            || bpp.is_none()
            || frame.stride_bytes < frame.width * bpp.unwrap_or(1)
            || frame.stride_bytes.checked_mul(frame.height) != Some(frame.data.len() as u64)
            || frame.data.len() as u64 > MAX_PAYLOAD_BYTES
        {
            return Err("FRAME_PACKET_INVALID_GEOMETRY_OR_FORMAT".into());
        }
    } else if !frame.data.is_empty() {
        return Err("FRAME_PACKET_INVALID_EMPTY_FRAME".into());
    }
    let mut out = Vec::new();
    out.try_reserve_exact(HEADER_BYTES + frame.data.len())
        .map_err(|_| "FRAME_PACKET_ALLOCATION_FAILED".to_string())?;
    out.extend_from_slice(b"MIBF");
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&(HEADER_BYTES as u16).to_le_bytes());
    out.extend_from_slice(&u32::from(frame.valid).to_le_bytes());
    out.extend_from_slice(&pull_kind.to_le_bytes());
    // The legacy timestamp value is preserved losslessly, but its clock and
    // validity are UNKNOWN. Source/session/config IDs are not in the frame.
    // Reserved identity slots MUST stay zero until an accepted backend contract.
    let fields = if frame.valid {
        [frame.frame_index, frame.timestamp_ns, frame.width, frame.height,
         frame.pixel_format, frame.stride_bytes, frame.data.len() as u64, 0, 0, 0]
    } else { [0; 10] };
    for n in fields { out.extend_from_slice(&n.to_le_bytes()); }
    out.extend_from_slice(&frame.data);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(not(feature = "review-only"))]
    #[test]
    fn cpp_rust_binary_matches_shared_golden() {
        let frame = mib_bridge::ffi::contract_fixture_frame();
        let packet = encode(frame, 1).unwrap();
        let fixture: serde_json::Value = serde_json::from_str(include_str!(
            "../../../crates/mib-bridge/contract/fixtures/frame-v1.json")).unwrap();
        assert_eq!(hex::encode(packet), fixture["hex"].as_str().unwrap());
    }
    fn frame(index: u64, value: u8) -> Frame {
        Frame { valid: true, frame_index: index, timestamp_ns: u64::MAX,
            width: 2, height: 2, pixel_format: PIXEL_FORMAT_MONO8, stride_bytes: 2,
            data: vec![value; 4] }
    }
    #[test]
    fn interleaved_packets_own_their_metadata_and_pixels() {
        for i in 0..5000 {
            let a = encode(frame((1u64 << 53) + i, 11), 1).unwrap();
            let b = encode(frame(u64::MAX - i, 22), 2).unwrap();
            // Complete B before A. No mutable owner exists between responses.
            assert_eq!(&b[96..], &[22; 4]);
            assert_eq!(&a[96..], &[11; 4]);
            assert_eq!(u64::from_le_bytes(a[16..24].try_into().unwrap()), (1u64 << 53) + i);
            assert_eq!(a.len() + b.len(), 200);
        }
    }
    #[test]
    fn rejects_bad_geometry_and_unknown_formats_before_packet_allocation() {
        let mut f = frame(1, 1); f.stride_bytes = u64::MAX;
        assert!(encode(f, 1).is_err());
        let mut f = frame(1, 1); f.width = MAX_DIMENSION + 1;
        assert!(encode(f, 1).is_err());
        let mut f = frame(1, 1); f.pixel_format = 123;
        assert!(encode(f, 1).is_err());
        let mut f = frame(1, 1); f.valid = false;
        assert!(encode(f, 1).is_err());
        assert!(encode(frame(1, 1), MAX_PULL_KIND + 1).is_err());
    }
    #[test]
    fn rgb8_review_frames_carry_three_bytes_per_pixel() {
        // RGB8 needs stride = width*3 (ADR 0008 review pulls).
        let mut f = frame(7, 9);
        f.pixel_format = PIXEL_FORMAT_RGB8;
        assert!(encode(f.clone(), 3).is_err(), "mono stride with RGB8 format");
        f.stride_bytes = 6;
        f.data = vec![9; 12];
        let p = encode(f, 5).unwrap();
        assert_eq!(u64::from_le_bytes(p[48..56].try_into().unwrap()), PIXEL_FORMAT_RGB8);
        assert_eq!(u32::from_le_bytes(p[12..16].try_into().unwrap()), 5);
        assert_eq!(p.len(), 96 + 12);
    }
    #[test]
    fn pins_wire_header_and_exact_integer_encoding() {
        let p = encode(frame(u64::MAX, 11), 1).unwrap();
        assert_eq!(&p[..16], &[77,73,66,70,1,0,96,0,1,0,0,0,1,0,0,0]);
        assert_eq!(&p[16..32], &[255; 16]);
        assert_eq!(&p[72..96], &[0; 24]);
    }
}

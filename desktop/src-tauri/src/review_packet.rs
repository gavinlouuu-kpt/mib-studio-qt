//! YOFO Review frame packets (ADR 0014): immutable, explicitly little-endian
//! IPC frame responses for the review bridge's frames, thumbnail strips and
//! series images. The header is the bridge contract's frame packet
//! (`crates/mib-app-commands/src/frame_packet.rs`, same magic, version, layout
//! and limits); `review-contract.json` adds the review pull kinds and RGB8.
//! Capture identities (`capture_session`, `store_generation`) and the config
//! revision stay zero: review frames come from a recorded file.
use mib_bridge::review_ffi::ReviewFrame;

include!("review_packet_contract.rs");

/// Header facts shared with MIB Studio's packets (bridge-contract.json
/// `frame_packet`); `review_packet_contract_matches_bridge_contract` pins them.
pub const VERSION: u16 = 2;
pub const HEADER_BYTES: usize = 96;
pub const MAX_PAYLOAD_BYTES: u64 = 33554432;
pub const MAX_PIXELS: u64 = 16777216;
pub const MAX_DIMENSION: u64 = 8192;
/// JSON command responses carry the bridge contract's transport version.
pub const JSON_TRANSPORT_VERSION: u32 = 1;

/// Bytes per pixel of a review pixel format, or None when unknown.
fn bytes_per_pixel(pixel_format: u64) -> Option<u64> {
    match pixel_format {
        PIXEL_FORMAT_MONO8_LEGACY | PIXEL_FORMAT_MONO8 => Some(1),
        PIXEL_FORMAT_RGB8 => Some(3),
        _ => None,
    }
}

pub fn encode(frame: ReviewFrame, pull_kind: u32) -> Result<Vec<u8>, String> {
    if ![PULL_KIND_REVIEW, PULL_KIND_REVIEW_THUMBNAILS, PULL_KIND_REVIEW_SERIES].contains(&pull_kind) {
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
    // Review frames carry no timestamp or capture identity.
    let fields = if frame.valid {
        [frame.frame_index, 0, frame.width, frame.height,
         frame.pixel_format, frame.stride_bytes, frame.data.len() as u64, 0, 0, 0]
    } else { [0; 10] };
    for n in fields { out.extend_from_slice(&n.to_le_bytes()); }
    out.extend_from_slice(&frame.data);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(index: u64, value: u8) -> ReviewFrame {
        ReviewFrame { valid: true, frame_index: index, width: 2, height: 2,
            pixel_format: PIXEL_FORMAT_MONO8, stride_bytes: 2, data: vec![value; 4] }
    }

    #[test]
    fn review_packet_contract_matches_bridge_contract() {
        let bridge: serde_json::Value = serde_json::from_str(include_str!(
            "../../../crates/mib-bridge/contract/bridge-contract.json")).unwrap();
        let p = &bridge["frame_packet"];
        assert_eq!(p["version"].as_u64(), Some(VERSION as u64));
        assert_eq!(p["header_bytes"].as_u64(), Some(HEADER_BYTES as u64));
        assert_eq!(p["max_payload_bytes"].as_u64(), Some(MAX_PAYLOAD_BYTES));
        assert_eq!(p["max_pixels"].as_u64(), Some(MAX_PIXELS));
        assert_eq!(p["max_dimension"].as_u64(), Some(MAX_DIMENSION));
        assert_eq!(p["pull_kinds"]["review"].as_u64(), Some(PULL_KIND_REVIEW as u64));
        assert_eq!(bridge["json_transport"]["version"].as_u64(), Some(JSON_TRANSPORT_VERSION as u64));
    }

    #[test]
    fn pins_header_and_zero_identities() {
        let p = encode(frame(u64::MAX, 7), PULL_KIND_REVIEW).unwrap();
        assert_eq!(&p[0..4], b"MIBF");
        assert_eq!(u16::from_le_bytes(p[4..6].try_into().unwrap()), VERSION);
        assert_eq!(u64::from_le_bytes(p[16..24].try_into().unwrap()), u64::MAX);
        for offset in [24, 72, 80, 88] {
            assert_eq!(u64::from_le_bytes(p[offset..offset + 8].try_into().unwrap()), 0, "offset {offset}");
        }
        assert_eq!(&p[96..], &[7; 4]);
    }

    #[test]
    fn rgb8_frames_carry_three_bytes_per_pixel() {
        let mut f = frame(7, 9);
        f.pixel_format = PIXEL_FORMAT_RGB8;
        assert!(encode(f.clone(), PULL_KIND_REVIEW).is_err(), "mono stride with RGB8 format");
        f.stride_bytes = 6;
        f.data = vec![9; 12];
        let p = encode(f, PULL_KIND_REVIEW_THUMBNAILS).unwrap();
        assert_eq!(u64::from_le_bytes(p[48..56].try_into().unwrap()), PIXEL_FORMAT_RGB8);
        assert_eq!(u32::from_le_bytes(p[12..16].try_into().unwrap()), PULL_KIND_REVIEW_THUMBNAILS);
        assert_eq!(p.len(), 96 + 12);
    }

    #[test]
    fn rejects_live_kinds_bad_geometry_and_unknown_formats() {
        assert!(encode(frame(1, 1), 1).is_err(), "live pull kind");
        assert!(encode(frame(1, 1), 4).is_err(), "background pull kind");
        let mut f = frame(1, 1); f.width = MAX_DIMENSION + 1;
        assert!(encode(f, PULL_KIND_REVIEW).is_err());
        let mut f = frame(1, 1); f.pixel_format = 99;
        assert!(encode(f, PULL_KIND_REVIEW).is_err());
        let mut f = frame(1, 1); f.valid = false;
        assert!(encode(f, PULL_KIND_REVIEW).is_err(), "empty frame with pixels");
    }
}

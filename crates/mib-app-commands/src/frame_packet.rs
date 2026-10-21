//! Immutable, explicitly little-endian IPC frame response. No retained cache.
//! See bridge-contract.json/frame_packet; independent of science/ABI versions.
use mib_bridge::ffi::BridgeFrame;

include!("frame_packet_contract.rs");

pub fn encode(frame: BridgeFrame, pull_kind: u32) -> Result<Vec<u8>, String> {
    if !PULL_KINDS.contains(&pull_kind) {
        return Err("FRAME_PACKET_INVALID_SOURCE".into());
    }
    if frame.valid {
        if frame.width == 0 || frame.height == 0
            || frame.width > MAX_DIMENSION || frame.height > MAX_DIMENSION
            || frame.width.checked_mul(frame.height).filter(|n| *n <= MAX_PIXELS).is_none()
            || frame.stride_bytes < frame.width
            || frame.stride_bytes.checked_mul(frame.height) != Some(frame.data.len() as u64)
            || frame.data.len() as u64 > MAX_PAYLOAD_BYTES
            // Existing facade uses 0 for normalized Mono8 review/background.
            || ![0, 0x01080001].contains(&frame.pixel_format)
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
    // validity are UNKNOWN. Capture generation/store epoch are stamped with pixels;
    // processing configuration is inapplicable to raw frames and stays reserved.
    let fields = if frame.valid {
        [frame.frame_index, frame.timestamp_ns, frame.width, frame.height,
         frame.pixel_format, frame.stride_bytes, frame.data.len() as u64, frame.capture_session, 0, frame.store_generation]
    } else { [0; 10] };
    for n in fields { out.extend_from_slice(&n.to_le_bytes()); }
    out.extend_from_slice(&frame.data);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cpp_rust_binary_matches_shared_golden() {
        let frame = mib_bridge::ffi::contract_fixture_frame();
        let packet = encode(frame, 1).unwrap();
        let fixture: serde_json::Value = serde_json::from_str(include_str!(
            "../../mib-bridge/contract/fixtures/frame-v2.json")).unwrap();
        assert_eq!(hex::encode(packet), fixture["hex"].as_str().unwrap());
    }
    fn frame(index: u64, value: u8) -> BridgeFrame {
        BridgeFrame { valid: true, frame_index: index, timestamp_ns: u64::MAX,
            width: 2, height: 2, pixel_format: 0x01080001, stride_bytes: 2,
            data: vec![value; 4], ..Default::default() }
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
    fn acquisition_epochs_remain_exact_and_owned() {
        let mut f = frame(1, 8);
        f.capture_session = (1u64 << 53) + 1;
        f.store_generation = u64::MAX;
        let p = encode(f, 1).unwrap();
        assert_eq!(u64::from_le_bytes(p[72..80].try_into().unwrap()), (1u64 << 53) + 1);
        assert_eq!(u64::from_le_bytes(p[88..96].try_into().unwrap()), u64::MAX);
        assert_eq!(&p[80..88], &[0; 8]);
    }
    #[test]
    fn rejects_bad_geometry_and_non_mono8_before_packet_allocation() {
        let mut f = frame(1, 1); f.stride_bytes = u64::MAX;
        assert!(encode(f, 1).is_err());
        let mut f = frame(1, 1); f.width = MAX_DIMENSION + 1;
        assert!(encode(f, 1).is_err());
        let mut f = frame(1, 1); f.pixel_format = 123;
        assert!(encode(f, 1).is_err());
        let mut f = frame(1, 1); f.valid = false;
        assert!(encode(f, 1).is_err());
    }
    // ABI 41 (#716 A6a): a monitoring cell crop travels as pull kind 5 (its mask as 6), Mono8 with
    // zero capture identities; the shared golden is decoded by desktop/src/framePacket.test.ts.
    #[test]
    fn monitoring_cell_packet_matches_shared_golden() {
        let frame = BridgeFrame { valid: true, frame_index: 9007199254740993, timestamp_ns: u64::MAX,
            width: 3, height: 2, pixel_format: 0, stride_bytes: 3,
            data: vec![11, 22, 33, 44, 55, 66], ..Default::default() };
        let fixture: serde_json::Value = serde_json::from_str(include_str!(
            "../../mib-bridge/contract/fixtures/monitoring-cell-v2.json")).unwrap();
        let packet = encode(frame.clone(), PULL_KIND_MONITORING_CELL_IMAGE).unwrap();
        assert_eq!(hex::encode(&packet), fixture["hex"].as_str().unwrap());
        let mask = encode(frame, PULL_KIND_MONITORING_CELL_MASK).unwrap();
        assert_eq!(u32::from_le_bytes(mask[12..16].try_into().unwrap()), 6);
        // Not held: an empty packet of the requested kind, nothing else.
        let empty = encode(BridgeFrame::default(), PULL_KIND_MONITORING_CELL_IMAGE).unwrap();
        assert_eq!(empty.len(), HEADER_BYTES);
        assert_eq!(&empty[8..16], &[0, 0, 0, 0, 5, 0, 0, 0]);
        assert!(encode(BridgeFrame::default(), 7).is_err(), "an unknown pull kind is refused");
        assert!(encode(BridgeFrame::default(), 0).is_err());
    }
    #[test]
    fn pins_wire_header_and_exact_integer_encoding() {
        let p = encode(frame(u64::MAX, 11), 1).unwrap();
        assert_eq!(&p[..16], &[77,73,66,70,2,0,96,0,1,0,0,0,1,0,0,0]);
        assert_eq!(&p[16..32], &[255; 16]);
        assert_eq!(&p[72..96], &[0; 24]);
    }
}

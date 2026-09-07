//! Versioned lossless JSON boundary. This is marshaling, not run truth.
use mib_bridge::ffi::BridgeEvent;
use serde::{Serialize, Serializer};
use crate::frame_packet::{JSON_TRANSPORT_VERSION, MAX_EVENT_TEXT_BYTES};

pub fn serialize_u64<S: Serializer>(value: &u64, serializer: S) -> Result<S::Ok, S::Error> {
    serializer.serialize_str(&value.to_string())
}

#[derive(Serialize)]
pub struct EventEnvelope {
    transport_version: u32,
    events: Vec<ExactEvent>,
}
#[derive(Serialize)]
struct ExactEvent {
    kind: &'static str,
    u0: String, u1: String, u2: String, u3: String, u4: String, u5: String,
    f0: Option<f64>, f1: Option<f64>, f2: Option<f64>,
    b0: bool, b1: bool,
    text: String,
    text_truncated: bool,
    experiment_end_time_ns: String,
    experiment_dropped_valid: String,
    experiment_dropped_invalid: String,
    frame_byte_size: String,
}
fn finite(n: f64) -> Option<f64> { if n.is_finite() { Some(n) } else { None } }

pub fn encode(events: Vec<BridgeEvent>) -> EventEnvelope {
    EventEnvelope {
        transport_version: JSON_TRANSPORT_VERSION,
        events: events.into_iter().map(|e| {
            let mut text = e.text;
            let truncated = text.len() > MAX_EVENT_TEXT_BYTES;
            if truncated {
                let mut boundary = MAX_EVENT_TEXT_BYTES;
                while !text.is_char_boundary(boundary) { boundary -= 1; }
                text.truncate(boundary);
            }
            ExactEvent {
                kind: super::kind_name(e.kind),
                u0: e.u0.to_string(), u1: e.u1.to_string(), u2: e.u2.to_string(),
                u3: e.u3.to_string(), u4: e.u4.to_string(), u5: e.u5.to_string(),
                f0: finite(e.f0), f1: finite(e.f1), f2: finite(e.f2),
                b0: e.b0, b1: e.b1, text, text_truncated: truncated,
                experiment_end_time_ns: e.experiment_end_time_ns.to_string(),
                experiment_dropped_valid: e.experiment_dropped_valid.to_string(),
                experiment_dropped_invalid: e.experiment_dropped_invalid.to_string(),
                frame_byte_size: e.frame_byte_size.to_string(),
            }
        }).collect(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cpp_rust_json_matches_shared_golden() {
        let events = mib_bridge::ffi::contract_fixture_events();
        let actual = serde_json::to_value(encode(events)).unwrap();
        let expected: serde_json::Value = serde_json::from_str(include_str!(
            "../../../crates/mib-bridge/contract/fixtures/events-v1.json")).unwrap();
        assert_eq!(actual, expected);
    }
    #[test]
    fn long_utf8_detail_is_bounded_and_explicitly_marked() {
        let mut events = mib_bridge::ffi::contract_fixture_events();
        events[0].text = "影".repeat(5000);
        let json = serde_json::to_value(encode(events)).unwrap();
        assert!(json["events"][0]["text"].as_str().unwrap().len() <= MAX_EVENT_TEXT_BYTES);
        assert_eq!(json["events"][0]["text_truncated"], true);
    }
    #[derive(Serialize)]
    struct Number { #[serde(serialize_with = "serialize_u64")] value: u64 }
    #[test]
    fn json_integer_boundaries_are_strings() {
        for n in [(1u64 << 53)-1, 1u64 << 53, (1u64 << 53)+1, u64::MAX] {
            assert_eq!(serde_json::to_value(Number { value: n }).unwrap()["value"], n.to_string());
        }
    }
}

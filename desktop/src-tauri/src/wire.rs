//! Lossless integer marshaling shared by every JSON command response.
use serde::Serializer;

/// u64 as a canonical decimal string (JSON numbers lose precision above 2^53).
pub fn serialize_u64<S: Serializer>(value: &u64, serializer: S) -> Result<S::Ok, S::Error> {
    serializer.serialize_str(&value.to_string())
}

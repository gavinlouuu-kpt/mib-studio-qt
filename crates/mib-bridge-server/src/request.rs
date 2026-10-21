//! The `/ws` request frame, parsed from the client's raw text with duplicate object keys refused at
//! any depth (#788).
//!
//! A `serde_json::Value` keeps the last of two equal keys, so `{"paths":[a],"paths":[b]}` would
//! reach a command as `[b]` and the backend's strict parsers, which refuse duplicates, would only
//! see the collapsed re-serialisation. The frame is therefore parsed once, here, by a visitor that
//! builds the same `Value` the server used before and fails on the first repeated key in any
//! object: the frame itself, `args`, nested objects and objects inside arrays. Every command family
//! is covered because nothing reaches `Server::run` without passing through `parse`. There is no
//! second parse: the visitor builds the argument tree directly from the text, and the one extra
//! check is a key lookup in the map being built.
//!
//! JSON documents carried inside a string argument (`startRequestJson`, `apply_processing_config_json`)
//! are not part of the frame's structure; the backend parses those itself, with its own duplicate rule.

use std::collections::HashSet;
use std::fmt;

use serde::de::{self, Deserializer, MapAccess, SeqAccess, Visitor};
use serde::Deserialize;
use serde_json::{Map, Number, Value};

/// One client request: `{"request_id": n, "cmd": "...", "args": {...}}` (`args` optional).
#[derive(Debug)]
pub(crate) struct Request {
    pub request_id: u64,
    pub cmd: String,
    pub args: Value,
}

/// Parse a request frame. Malformed JSON, a wrong field type, a missing field, trailing text and a
/// duplicate key anywhere fail alike; the caller answers `BAD_REQUEST` and runs nothing.
pub(crate) fn parse(text: &str) -> Result<Request, serde_json::Error> {
    serde_json::from_str(text)
}

/// The request id of a frame that `parse` refused, when the frame names exactly one: the refusal is
/// then addressed to the call that is waiting for it (the client drops replies without an id).
/// `None` for text that is not a JSON object (a derived struct would also take `[5]` as id 5), has
/// no numeric `request_id`, or repeats it.
pub(crate) fn request_id_of(text: &str) -> Option<u64> {
    struct Id(Option<u64>);

    impl<'de> Deserialize<'de> for Id {
        fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
            struct IdVisitor;

            impl<'de> Visitor<'de> for IdVisitor {
                type Value = Id;

                fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                    f.write_str("a request object")
                }

                fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Id, A::Error> {
                    let mut id = None;
                    while let Some(key) = map.next_key::<String>()? {
                        if key != "request_id" {
                            map.next_value::<de::IgnoredAny>()?;
                        } else if id.is_some() {
                            return Err(duplicate(&key));
                        } else {
                            id = Some(map.next_value::<u64>()?);
                        }
                    }
                    Ok(Id(id))
                }
            }

            deserializer.deserialize_map(IdVisitor)
        }
    }

    serde_json::from_str::<Id>(text).ok().and_then(|id| id.0)
}

fn duplicate<E: de::Error>(key: &str) -> E {
    E::custom(format_args!("duplicate key `{key}`"))
}

impl<'de> Deserialize<'de> for Request {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct RequestVisitor;

        impl<'de> Visitor<'de> for RequestVisitor {
            type Value = Request;

            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("a request object")
            }

            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Request, A::Error> {
                let (mut request_id, mut cmd, mut args) = (None, None, None);
                // Fields the protocol does not define are ignored, as before, but checked too.
                let mut other = HashSet::new();
                while let Some(key) = map.next_key::<String>()? {
                    match key.as_str() {
                        "request_id" if request_id.is_some() => return Err(duplicate(&key)),
                        "request_id" => request_id = Some(map.next_value::<u64>()?),
                        "cmd" if cmd.is_some() => return Err(duplicate(&key)),
                        "cmd" => cmd = Some(map.next_value::<String>()?),
                        "args" if args.is_some() => return Err(duplicate(&key)),
                        "args" => args = Some(map.next_value::<StrictValue>()?.0),
                        _ if other.contains(&key) => return Err(duplicate(&key)),
                        _ => {
                            map.next_value::<StrictValue>()?;
                            other.insert(key);
                        }
                    }
                }
                Ok(Request {
                    request_id: request_id.ok_or_else(|| de::Error::missing_field("request_id"))?,
                    cmd: cmd.ok_or_else(|| de::Error::missing_field("cmd"))?,
                    args: args.unwrap_or(Value::Null),
                })
            }
        }

        deserializer.deserialize_map(RequestVisitor)
    }
}

/// A JSON value with every object's keys unique; otherwise exactly `serde_json::Value`.
struct StrictValue(Value);

impl<'de> Deserialize<'de> for StrictValue {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(StrictVisitor).map(StrictValue)
    }
}

struct StrictVisitor;

impl<'de> Visitor<'de> for StrictVisitor {
    type Value = Value;

    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("any JSON value")
    }

    fn visit_bool<E>(self, v: bool) -> Result<Value, E> {
        Ok(Value::Bool(v))
    }

    fn visit_i64<E>(self, v: i64) -> Result<Value, E> {
        Ok(Value::Number(v.into()))
    }

    fn visit_u64<E>(self, v: u64) -> Result<Value, E> {
        Ok(Value::Number(v.into()))
    }

    fn visit_f64<E>(self, v: f64) -> Result<Value, E> {
        // As serde_json's own visitor: a non-finite float (never produced by its parser) is null.
        Ok(Number::from_f64(v).map_or(Value::Null, Value::Number))
    }

    fn visit_str<E>(self, v: &str) -> Result<Value, E> {
        Ok(Value::String(v.to_owned()))
    }

    fn visit_string<E>(self, v: String) -> Result<Value, E> {
        Ok(Value::String(v))
    }

    fn visit_none<E>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }

    fn visit_some<D: Deserializer<'de>>(self, deserializer: D) -> Result<Value, D::Error> {
        StrictValue::deserialize(deserializer).map(|v| v.0)
    }

    fn visit_unit<E>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Value, A::Error> {
        let mut items = Vec::with_capacity(seq.size_hint().unwrap_or(0).min(4096));
        while let Some(StrictValue(item)) = seq.next_element()? {
            items.push(item);
        }
        Ok(Value::Array(items))
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Value, A::Error> {
        let mut object = Map::new();
        while let Some(key) = map.next_key::<String>()? {
            // Checked before the value is read, so the error's position is the repeated key's.
            if object.contains_key(&key) {
                return Err(duplicate(&key));
            }
            let StrictValue(value) = map.next_value()?;
            object.insert(key, value);
        }
        Ok(Value::Object(object))
    }
}

#[cfg(test)]
mod tests {
    use super::{parse, request_id_of};
    use serde_json::{json, Value};

    fn refused(text: &str) -> String {
        parse(text).map(|r| format!("{r:?}")).expect_err(text).to_string()
    }

    // #788: a repeated key anywhere in the frame is an error, never "the last one wins".
    #[test]
    fn duplicate_keys_are_refused_at_any_depth() {
        let cases = [
            // The frame's own fields, and a field the protocol does not define.
            (r#"{"request_id":1,"cmd":"a","cmd":"b","args":{}}"#, "`cmd`"),
            (r#"{"request_id":1,"request_id":2,"cmd":"a"}"#, "`request_id`"),
            (r#"{"request_id":1,"cmd":"a","args":{},"args":{}}"#, "`args`"),
            (r#"{"request_id":1,"cmd":"a","x":1,"x":2}"#, "`x`"),
            // Top level of args (the Codex case on #786), nested, inside arrays of objects.
            (r#"{"request_id":1,"cmd":"run_quick_look","args":{"paths":["a"],"paths":["b"]}}"#, "`paths`"),
            (r#"{"request_id":1,"cmd":"c","args":{"reference":{"path":"p","run_id":"1","path":"q"}}}"#, "`path`"),
            (r#"{"request_id":1,"cmd":"c","args":{"a":{"b":{"c":{"d":1,"d":1}}}}}"#, "`d`"),
            (r#"{"request_id":1,"cmd":"c","args":{"list":[{"k":1},{"k":2,"k":3}]}}"#, "`k`"),
            (r#"{"request_id":1,"cmd":"c","args":[[{"k":1,"k":1}]]}"#, "`k`"),
            (r#"{"request_id":1,"cmd":"c","x":[{"y":{"z":0,"z":0}}]}"#, "`z`"),
            // Equal values, and keys that are equal once unescaped, are still duplicates.
            (r#"{"request_id":1,"cmd":"c","args":{"a":1,"a":1}}"#, "`a`"),
        ];
        for (text, key) in cases {
            let e = refused(text);
            assert!(e.starts_with(&format!("duplicate key {key} at line 1 column")), "{text}: {e}");
        }
    }

    #[test]
    fn a_request_without_duplicates_parses_as_before() {
        let text = r#"{"request_id":7,"cmd":"run_quick_look","args":{"paths":["a","b"],
            "reference":{"path":"a","run_id":"18446744073709551615"},
            "same":[{"k":1},{"k":2}],"nested":{"k":{"k":{"k":-1.5}}},"n":null,"t":true,"big":18446744073709551615}}"#;
        let r = parse(text).unwrap();
        assert_eq!((r.request_id, r.cmd.as_str()), (7, "run_quick_look"));
        // The very same tree serde_json builds (a key may recur in sibling or nested objects).
        let expected: Value = serde_json::from_str::<Value>(text).unwrap()["args"].clone();
        assert_eq!(r.args, expected);
        let r = parse(r#"{"cmd":"init","request_id":0,"extra":{"a":1}}"#).unwrap();
        assert_eq!((r.request_id, r.cmd.as_str(), r.args), (0, "init", Value::Null));
        assert_eq!(parse(r#"{"request_id":1,"cmd":"c","args":null}"#).unwrap().args, Value::Null);
    }

    // Everything that was BAD_REQUEST before still is.
    #[test]
    fn malformed_frames_are_still_refused() {
        for (text, why) in [
            ("not json", "expected"),
            (r#"{"request_id":1,"cmd":"c""#, "EOF"),
            (r#"{"request_id":1,"cmd":"c"} x"#, "trailing"),
            (r#"{"cmd":"c"}"#, "missing field `request_id`"),
            (r#"{"request_id":1}"#, "missing field `cmd`"),
            (r#"{"request_id":-1,"cmd":"c"}"#, "invalid value"),
            (r#"{"request_id":1,"cmd":3}"#, "invalid type"),
            (r#"[1,"c",{}]"#, "expected a request object"),
        ] {
            let e = refused(text);
            assert!(e.contains(why), "{text}: {e}");
        }
    }

    #[test]
    fn a_refusal_names_the_request_when_the_frame_has_one_id() {
        assert_eq!(request_id_of(r#"{"request_id":5,"cmd":"c","args":{"a":1,"a":2}}"#), Some(5));
        assert_eq!(request_id_of(r#"{"cmd":"c","cmd":"d","request_id":6}"#), Some(6));
        assert_eq!(request_id_of(r#"{"request_id":5,"request_id":6,"cmd":"c"}"#), None, "which call is it?");
        assert_eq!(request_id_of(r#"{"request_id":"5","cmd":"c"}"#), None);
        assert_eq!(request_id_of("not json"), None);
        // An array names no request, though a derived struct would read it positionally (Codex, #792).
        assert_eq!(request_id_of("[5]"), None);
        assert_eq!(request_id_of(r#"[5,"take_control",{"a":1,"a":2}]"#), None);
        // A duplicate elsewhere in a frame that is otherwise readable still names its request.
        assert_eq!(request_id_of(r#"{"x":{"a":1,"a":2},"request_id":8}"#), Some(8));
        assert_eq!(request_id_of(r#"{"cmd":"c"}"#), None);
    }

    /// `cargo test -p mib-bridge-server --lib -- --ignored --nocapture one_megabyte`: the strict parse
    /// against the plain `serde_json` parse it replaces, on a 1 MB request.
    #[test]
    #[ignore]
    fn one_megabyte_request_timing() {
        #[derive(serde::Deserialize)]
        #[allow(dead_code)]
        struct Plain {
            request_id: u64,
            cmd: String,
            #[serde(default)]
            args: Value,
        }
        let shapes: [(&str, Value); 2] = [
            ("many objects", json!({ "rows": (0..18_000).map(|i| json!({ "id": i, "name": format!("cell-{i}"), "area": i as f64 * 0.5, "ok": i % 2 == 0 })).collect::<Vec<_>>() })),
            ("one long string", json!({ "paths": ["x".repeat(1 << 20)] })),
        ];
        for (shape, args) in shapes {
            let text = json!({ "request_id": 1, "cmd": "c", "args": args }).to_string();
            let time = |f: &dyn Fn()| {
                f(); // warm up
                let n = 30;
                let start = std::time::Instant::now();
                for _ in 0..n {
                    f();
                }
                start.elapsed() / n
            };
            let plain = time(&|| {
                std::hint::black_box(serde_json::from_str::<Plain>(&text).unwrap());
            });
            let strict = time(&|| {
                std::hint::black_box(parse(&text).unwrap());
            });
            println!("{shape}: {} bytes, plain {plain:?}, strict {strict:?}", text.len());
        }
    }
}

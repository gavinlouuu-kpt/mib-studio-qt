//! Boot service toggles (Qt-parity HW-038): Settings > Boot Service Toggles.
//!
//! Qt persists the services to leave out of the next launch (`Startup/DisabledServices`) and
//! hands them to the backend as `MIB_DISABLED_SERVICES` before it initializes
//! (`main.cpp: applyBootDisabledServicesFromSettings`). The desktop shell keeps the same set in
//! its preferences document under [`PREFERENCE_KEY`] and applies it the same way: once per
//! process, before the backend initializes, unless the environment already names services.
//! The logic here is pure so it is tested without a window; the shell reads the preferences file
//! and calls [`apply_preferences`].

use serde_json::Value;

/// The preferences document key: an array of [`BOOT_SERVICE_TOKENS`].
pub const PREFERENCE_KEY: &str = "bootDisabledServices";
/// The variable `AppBackend::initialize` reads.
pub const ENV_NAME: &str = "MIB_DISABLED_SERVICES";

/// Qt's Boot Service Toggles list, in its order (`MainWindow.cpp`). The backend accepts more
/// (`all`, `dot_grid`, `yolo`, `camera`): the shell only ever writes these.
pub const BOOT_SERVICE_TOKENS: [&str; 8] =
    ["sqlite", "hdf5", "processing", "autofocus", "trigger", "capture", "playback", "auto_update"];

/// What to do with the environment.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Decision {
    /// The environment already names services (`value`): it wins, the preference is not applied.
    Environment { value: String },
    /// Set `MIB_DISABLED_SERVICES` to `value` (comma-joined, [`BOOT_SERVICE_TOKENS`] order).
    Apply { value: String },
    /// Nothing is disabled and the environment is silent: leave it unset.
    Nothing,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Resolution {
    pub decision: Decision,
    /// Preference entries that are not known tokens (as written), left out of `Apply`.
    pub ignored: Vec<String>,
}

impl Resolution {
    /// The shell-log line saying which source applied.
    pub fn log_line(&self) -> String {
        let ignored = if self.ignored.is_empty() {
            String::new()
        } else {
            format!("; ignored unknown {PREFERENCE_KEY} entries: {}", self.ignored.join(", "))
        };
        match &self.decision {
            Decision::Environment { value } => format!(
                "{ENV_NAME}={value} comes from the environment and wins; the desktop preference {PREFERENCE_KEY} is not applied{ignored}"
            ),
            Decision::Apply { value } => {
                format!("{ENV_NAME}={value} applied from the desktop preference {PREFERENCE_KEY}{ignored}")
            }
            Decision::Nothing => format!("no boot services disabled ({ENV_NAME} unset, no {PREFERENCE_KEY} entries){ignored}"),
        }
    }
}

/// Qt's token normalization (`normalizeDisabledServicesCsv`): trimmed, lower case, `-` as `_`.
fn normalize(token: &str) -> String {
    token.trim().to_lowercase().replace('-', "_")
}

/// The known tokens of the preference, in [`BOOT_SERVICE_TOKENS`] order and without duplicates,
/// and the entries that were ignored (unknown tokens, non-string entries; a value that is not an
/// array is ignored whole). A missing key is not an error: nothing is disabled.
pub fn tokens_from_preferences(preferences: &Value) -> (Vec<&'static str>, Vec<String>) {
    let Some(value) = preferences.get(PREFERENCE_KEY) else { return (Vec::new(), Vec::new()) };
    let Some(entries) = value.as_array() else { return (Vec::new(), vec![value.to_string()]) };
    let mut wanted = [false; BOOT_SERVICE_TOKENS.len()];
    let mut ignored = Vec::new();
    for entry in entries {
        let known = entry
            .as_str()
            .map(normalize)
            .and_then(|token| BOOT_SERVICE_TOKENS.iter().position(|known| *known == token));
        match known {
            Some(index) => wanted[index] = true,
            None => ignored.push(entry.as_str().map_or_else(|| entry.to_string(), str::to_string)),
        }
    }
    let tokens = BOOT_SERVICE_TOKENS.iter().zip(wanted).filter(|(_, on)| *on).map(|(token, _)| *token).collect();
    (tokens, ignored)
}

/// Decide what the environment should become. `environment` is the current value of
/// [`ENV_NAME`]; empty counts as unset, as in Qt (`qEnvironmentVariableIsEmpty`).
pub fn resolve(preferences: &Value, environment: Option<&str>) -> Resolution {
    let (tokens, ignored) = tokens_from_preferences(preferences);
    let decision = match environment.map(str::trim).filter(|value| !value.is_empty()) {
        Some(value) => Decision::Environment { value: value.to_string() },
        None if tokens.is_empty() => Decision::Nothing,
        None => Decision::Apply { value: tokens.join(",") },
    };
    Resolution { decision, ignored }
}

/// [`resolve`] against a real or fake environment: `set` is called only for [`Decision::Apply`].
/// Returns the log line.
pub fn apply_with(
    preferences: &Value,
    get: impl Fn(&str) -> Option<String>,
    mut set: impl FnMut(&str, &str),
) -> String {
    let resolution = resolve(preferences, get(ENV_NAME).as_deref());
    if let Decision::Apply { value } = &resolution.decision {
        set(ENV_NAME, value);
    }
    resolution.log_line()
}

/// Apply the preference to this process's environment. Call it before the backend initializes
/// (`AppBackend::initialize` reads the variable once) and log the returned line.
pub fn apply_preferences(preferences: &Value) -> String {
    apply_with(preferences, |name| std::env::var(name).ok(), set_process_env)
}

/// Set a variable so both Rust and the C++ backend see it. On Windows the C runtime keeps its own
/// copy of the environment, so `std::getenv` in the backend would not see `SetEnvironmentVariable`
/// (the Rust default): `_putenv_s` updates the runtime's copy and the process environment.
fn set_process_env(name: &str, value: &str) {
    std::env::set_var(name, value);
    #[cfg(windows)]
    {
        use std::ffi::CString;
        use std::os::raw::{c_char, c_int};
        extern "C" {
            fn _putenv_s(name: *const c_char, value: *const c_char) -> c_int;
        }
        if let (Ok(name), Ok(value)) = (CString::new(name), CString::new(value)) {
            // SAFETY: both are valid NUL-terminated strings that outlive the call.
            unsafe {
                _putenv_s(name.as_ptr(), value.as_ptr());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::cell::RefCell;

    fn apply(preferences: &Value, environment: Option<&str>) -> Resolution {
        resolve(preferences, environment)
    }

    #[test]
    fn the_token_list_is_the_one_in_the_contract() {
        let contract: Value = serde_json::from_str(include_str!("../../mib-bridge/contract/bridge-contract.json")).unwrap();
        let listed: Vec<&str> =
            contract["hardware_parity"]["boot_service_tokens"].as_array().unwrap().iter().map(|t| t.as_str().unwrap()).collect();
        assert_eq!(listed, BOOT_SERVICE_TOKENS);
    }

    #[test]
    fn preferences_become_the_comma_list_in_qt_order() {
        let r = apply(&json!({ "bootDisabledServices": ["playback", "sqlite", "auto_update"] }), None);
        assert_eq!(r.decision, Decision::Apply { value: "sqlite,playback,auto_update".to_string() });
        assert!(r.ignored.is_empty());
        let line = r.log_line();
        assert!(line.contains("MIB_DISABLED_SERVICES=sqlite,playback,auto_update applied from the desktop preference"), "{line}");
    }

    #[test]
    fn the_environment_wins_over_the_preference_and_says_so() {
        let r = apply(&json!({ "bootDisabledServices": ["sqlite"] }), Some("trigger,hdf5"));
        assert_eq!(r.decision, Decision::Environment { value: "trigger,hdf5".to_string() });
        assert!(r.log_line().contains("comes from the environment and wins"), "{}", r.log_line());
        // An empty (or blank) variable is not set, as in Qt: the preference applies.
        for empty in [Some(""), Some("  "), None] {
            let r = apply(&json!({ "bootDisabledServices": ["sqlite"] }), empty);
            assert_eq!(r.decision, Decision::Apply { value: "sqlite".to_string() }, "{empty:?}");
        }
    }

    #[test]
    fn unknown_and_malformed_entries_are_ignored_and_reported() {
        let r = apply(&json!({ "bootDisabledServices": ["Sqlite", " HDF5 ", "dot-grid", "all", 7, "sqlite", "auto-update", "bogus"] }), None);
        // Case and spacing are normalized, `-` is `_`, duplicates collapse; `all`, `dot_grid` and the
        // rest are not in the toggles list, so they are not written.
        assert_eq!(r.decision, Decision::Apply { value: "sqlite,hdf5,auto_update".to_string() });
        assert_eq!(r.ignored, vec!["dot-grid", "all", "7", "bogus"]);
        assert!(r.log_line().contains("ignored unknown bootDisabledServices entries: dot-grid, all, 7, bogus"), "{}", r.log_line());
    }

    #[test]
    fn nothing_to_disable_leaves_the_environment_alone() {
        for prefs in [json!({}), json!({ "bootDisabledServices": [] }), json!({ "bootDisabledServices": ["bogus"] }), json!({ "bootDisabledServices": "sqlite" }), json!(null)] {
            assert_eq!(apply(&prefs, None).decision, Decision::Nothing, "{prefs}");
        }
        assert_eq!(apply(&json!({ "bootDisabledServices": "sqlite" }), None).ignored, vec!["\"sqlite\""]);
    }

    #[test]
    fn apply_with_sets_the_variable_only_when_the_preference_applies() {
        let set = RefCell::new(Vec::new());
        let prefs = json!({ "bootDisabledServices": ["capture"] });
        let line = apply_with(&prefs, |_| None, |n, v| set.borrow_mut().push((n.to_string(), v.to_string())));
        assert_eq!(*set.borrow(), [("MIB_DISABLED_SERVICES".to_string(), "capture".to_string())]);
        assert!(line.contains("applied from the desktop preference"), "{line}");

        set.borrow_mut().clear();
        apply_with(&prefs, |_| Some("trigger".to_string()), |n, v| set.borrow_mut().push((n.to_string(), v.to_string())));
        assert!(set.borrow().is_empty(), "the environment wins: nothing is set");
    }
}

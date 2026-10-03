// Build script for the Rust <-> C++ bridge (epic #246, ADR 0003; review
// bridge ADR 0008).
//
// Two cxx bridge modules live in this crate:
//   - `ffi` (src/lib.rs + shim.cpp): BackendBridge over AppBackend/
//     BackendFacade — links libmib_backend.a and everything behind it.
//   - `review_ffi` (src/review_bridge.rs + review_shim.cpp): ReviewBridge over
//     ReviewSession — links libmib_review_core.a + libmib_processing.a only.
// Without features both are compiled. With `review-only` just the review
// bridge is, so the YOFO Review binary carries no camera, serial, SQLite,
// curl or Sentry code.
//
// 1. Ensures the Qt-free static archives exist by driving the
//    `linux-backend-only` CMake preset (skippable with MIB_BRIDGE_NO_CMAKE=1
//    when the caller has already built them, e.g. a CI job that ran cmake).
// 2. Compiles the cxx bridges + shims.
// 3. Links the static archives and their system dependencies (OpenCV / HDF5 /
//    SQLite / spdlog / fmt / crypto) — no Qt, no webkit, no display.
//
// Windows (MSVC/Conan tree): the same static libraries live in build/Release
// and their ~190 transitive import libraries are read from
// build/mib-bridge-link-manifest.json, written by
// tools/gen_bridge_link_manifest.py from the CMake-generated backend test
// project. MIB_BRIDGE_NO_CMAKE is implied: build the backend with the
// windows-default preset first.
//
// Review-only builds on macOS / Windows (YOFO Review): the macos-review-core
// / windows-review-core presets build the review core against static Conan
// dependencies plus `mib_review_link_probe`, and
// tools/gen_review_link_manifest.py records that probe's CMake-resolved link
// line and compile settings ("format": "mib-review-link-v1"). build.rs
// replays it verbatim — archives, frameworks, system libraries, in order —
// from MIB_BRIDGE_LINK_MANIFEST or build/review-core/. Linux uses it too when
// MIB_BRIDGE_LINK_MANIFEST points at one.

use std::path::{Path, PathBuf};
#[cfg(not(windows))]
use std::process::Command;

fn repo_root() -> PathBuf {
    // crate lives at <repo>/crates/mib-bridge
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    manifest
        .parent()
        .and_then(Path::parent)
        .expect("crate is two levels below repo root")
        .to_path_buf()
}

fn review_only() -> bool {
    std::env::var_os("CARGO_FEATURE_REVIEW_ONLY").is_some()
}

/// Static archives this build links, in link order (dependents first).
fn archives() -> Vec<&'static str> {
    if review_only() {
        vec!["mib_review_core", "mib_processing"]
    } else {
        vec!["mib_backend", "mib_review_core", "mib_processing", "oeabt_serial", "oeabt_core"]
    }
}

fn bridge_sources() -> Vec<&'static str> {
    if review_only() {
        vec!["src/review_bridge.rs"]
    } else {
        vec!["src/lib.rs", "src/review_bridge.rs"]
    }
}

fn shim_sources() -> Vec<&'static str> {
    if review_only() {
        vec!["src/review_shim.cpp"]
    } else {
        vec!["src/shim.cpp", "src/review_shim.cpp"]
    }
}

#[cfg(not(windows))]
fn ensure_backend_built(repo: &Path, build_dir: &Path) {
    let libs: Vec<PathBuf> = archives().iter().map(|l| build_dir.join(format!("lib{l}.a"))).collect();
    let all_exist = || libs.iter().all(|p| p.exists());

    if std::env::var("MIB_BRIDGE_NO_CMAKE").is_ok() {
        if !all_exist() {
            panic!(
                "MIB_BRIDGE_NO_CMAKE set but backend archives are missing in {}",
                build_dir.display()
            );
        }
        return;
    }

    // Configure (idempotent) then build only the archives this bridge needs.
    let configure = Command::new("cmake")
        .current_dir(repo)
        .args(["--preset", "linux-backend-only"])
        .status();
    let mut args = vec!["--build", "--preset", "linux-backend-only-build", "--target"];
    args.extend(archives().iter().filter(|l| !l.starts_with("oeabt")));
    let built = Command::new("cmake").current_dir(repo).args(&args).status();

    let ok = matches!(configure, Ok(s) if s.success()) && matches!(built, Ok(s) if s.success());

    if !ok {
        if all_exist() {
            println!(
                "cargo:warning=cmake backend build failed but archives exist; \
                 linking existing {}",
                build_dir.display()
            );
        } else {
            panic!("failed to build the backend archives via cmake preset");
        }
    }
}

fn compile_bridges(configure: impl FnOnce(&mut cc::Build)) {
    let mut bridge_build = cxx_build::bridges(bridge_sources());
    if std::env::var_os("CARGO_FEATURE_CONTRACT_FIXTURES").is_some() {
        bridge_build.define("MIB_BRIDGE_CONTRACT_FIXTURES", None);
    }
    for src in shim_sources() {
        bridge_build.file(src);
    }
    configure(&mut bridge_build);
    bridge_build.compile("mib_bridge_shim");
}

#[cfg(windows)]
fn windows_build(repo: &Path, include_dir: &Path) {
    // Windows: link against the MSVC/Conan build tree. CMake alone knows the
    // ~190 import libraries `mib_backend.lib` pulls in, so a manifest generated
    // from the backend-only test project by tools/gen_bridge_link_manifest.py
    // carries the link line and compile settings (see that script's docstring).
    let manifest_path = std::env::var("MIB_BRIDGE_LINK_MANIFEST")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo.join("build/mib-bridge-link-manifest.json"));
    println!("cargo:rerun-if-env-changed=MIB_BRIDGE_LINK_MANIFEST");
    println!("cargo:rerun-if-changed={}", manifest_path.display());
    let text = std::fs::read_to_string(&manifest_path).unwrap_or_else(|e| {
        panic!(
            "Windows bridge build needs {} (run `python tools/gen_bridge_link_manifest.py` \
             after building mib_backend, mib_review_core, mib_processing and mib_backend_smoke_test): {e}",
            manifest_path.display()
        )
    });
    let manifest: serde_json::Value = serde_json::from_str(&text).expect("link manifest is JSON");
    let strings = |key: &str| -> Vec<String> {
        manifest[key]
            .as_array()
            .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
            .unwrap_or_default()
    };

    let include_dirs = strings("include_dirs");
    let defines = strings("defines");
    compile_bridges(|b| {
        b.flag("/std:c++17")
            .flag("/EHsc")
            .flag("/utf-8")
            .flag("/Zc:__cplusplus")
            .define("NOMINMAX", None)
            .define("WIN32_LEAN_AND_MEAN", None)
            .include(include_dir);
        for d in &include_dirs {
            b.include(d);
        }
        for def in &defines {
            match def.split_once('=') {
                Some((k, v)) => {
                    b.define(k, v);
                }
                None => {
                    b.define(def, None);
                }
            }
        }
    });

    for d in strings("lib_dirs") {
        println!("cargo:rustc-link-search=native={d}");
    }
    // Order matters for static archives: the backend before its dependencies,
    // exactly as CMake linked the reference test. The review-only build keeps
    // the manifest's system libraries but drops the backend archives.
    let wanted = archives();
    for lib in strings("libs") {
        let is_ours = matches!(
            lib.as_str(),
            "mib_backend" | "mib_review_core" | "mib_processing" | "oeabt_serial" | "oeabt_core"
        );
        if is_ours {
            if wanted.contains(&lib.as_str()) {
                println!("cargo:rustc-link-lib=static={lib}");
            }
        } else {
            println!("cargo:rustc-link-lib={lib}");
        }
    }
    for lib in wanted {
        let dir = manifest["runtime_dirs"][0].as_str().unwrap_or("build/Release");
        println!("cargo:rerun-if-changed={dir}/{lib}.lib");
    }
}

const REVIEW_MANIFEST_FORMAT: &str = "mib-review-link-v1";

/// The review-core link manifest, when this build should use one: an explicit
/// MIB_BRIDGE_LINK_MANIFEST in that format, else build/review-core/'s for
/// review-only builds on macOS (required there) and Windows (when present).
fn review_manifest(repo: &Path) -> Option<(PathBuf, serde_json::Value)> {
    println!("cargo:rerun-if-env-changed=MIB_BRIDGE_LINK_MANIFEST");
    let default = repo.join("build/review-core/mib-bridge-link-manifest.json");
    let path = match std::env::var_os("MIB_BRIDGE_LINK_MANIFEST") {
        Some(p) => PathBuf::from(p),
        None if review_only() && cfg!(target_os = "macos") => default,
        None if review_only() && cfg!(windows) && default.is_file() => default,
        None => return None,
    };
    println!("cargo:rerun-if-changed={}", path.display());
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| {
        panic!(
            "review bridge link manifest {} unreadable ({e}); build the review core with the \
             <os>-review-core preset and run `python3 tools/gen_review_link_manifest.py` \
             (docs/howto/macos-build.md, docs/howto/build-installer.md)",
            path.display()
        )
    });
    let manifest: serde_json::Value = serde_json::from_str(&text).expect("link manifest is JSON");
    if manifest["format"].as_str() != Some(REVIEW_MANIFEST_FORMAT) {
        // The Windows MIB Studio manifest (gen_bridge_link_manifest.py).
        return None;
    }
    assert!(
        review_only(),
        "{} is a review-core link manifest; it links only the review bridge — build with \
         --features review-only (YOFO Review) or unset MIB_BRIDGE_LINK_MANIFEST",
        path.display()
    );
    Some((path, manifest))
}

fn manifest_build(include_dir: &Path, manifest: &serde_json::Value) {
    let strings = |key: &str| -> Vec<String> {
        manifest[key]
            .as_array()
            .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
            .unwrap_or_default()
    };
    let include_dirs = strings("include_dirs");
    let defines = strings("defines");
    compile_bridges(|b| {
        b.include(include_dir);
        if cfg!(target_env = "msvc") {
            b.flag("/std:c++17")
                .flag("/EHsc")
                .flag("/utf-8")
                .flag("/Zc:__cplusplus")
                .define("NOMINMAX", None)
                .define("WIN32_LEAN_AND_MEAN", None);
        } else {
            b.flag_if_supported("-std=c++17");
        }
        for d in &include_dirs {
            b.include(d);
        }
        for def in &defines {
            match def.split_once('=') {
                Some((k, v)) => {
                    b.define(k, v);
                }
                None => {
                    b.define(def, None);
                }
            }
        }
    });

    for d in strings("search_dirs") {
        println!("cargo:rustc-link-search=native={d}");
    }
    // Static archives are not bundled into the rlib (-bundle): they reach the
    // final link once, in CMake's order, after the shim that needs them.
    for entry in manifest["link"].as_array().into_iter().flatten() {
        let name = entry["name"].as_str().unwrap_or_default();
        let dir = entry["dir"].as_str().unwrap_or_default();
        match entry["kind"].as_str() {
            Some("static") => {
                println!("cargo:rustc-link-lib=static:-bundle={name}");
                if matches!(name, "mib_review_core" | "mib_processing") {
                    let file = if cfg!(target_env = "msvc") { format!("{name}.lib") } else { format!("lib{name}.a") };
                    println!("cargo:rerun-if-changed={}", Path::new(dir).join(file).display());
                }
            }
            Some("framework") => println!("cargo:rustc-link-lib=framework={name}"),
            _ => println!("cargo:rustc-link-lib=dylib={name}"),
        }
    }
}

fn main() {
    let repo = repo_root();
    let include_dir = repo.join("include");

    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_CONTRACT_FIXTURES");
    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_REVIEW_ONLY");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=src/shim.cpp");
    println!("cargo:rerun-if-changed=src/shim.h");
    println!("cargo:rerun-if-changed=src/review_bridge.rs");
    println!("cargo:rerun-if-changed=src/review_shim.cpp");
    println!("cargo:rerun-if-changed=src/review_shim.h");
    println!("cargo:rerun-if-env-changed=MIB_BRIDGE_NO_CMAKE");

    if let Some((_, manifest)) = review_manifest(&repo) {
        manifest_build(&include_dir, &manifest);
        return;
    }

    #[cfg(windows)]
    {
        windows_build(&repo, &include_dir);
    }

    #[cfg(not(windows))]
    {
        let build_dir = repo.join("build/linux-backend");
        ensure_backend_built(&repo, &build_dir);

        compile_bridges(|b| {
            b.flag_if_supported("-std=c++17")
                .include(&include_dir)
                .include("/usr/include/opencv4");
        });

        // Relink when the archives change (e.g. a facade edit) so a stale
        // build dir can't silently keep an old symbol set.
        println!("cargo:rustc-link-search=native={}", build_dir.display());
        for lib in archives() {
            println!("cargo:rerun-if-changed={}/lib{lib}.a", build_dir.display());
            println!("cargo:rustc-link-lib=static={lib}");
        }

        if !review_only() {
            // sentry-native (CrashReporter): the backend-only preset builds it
            // as a static archive under _deps when MIB_USE_SENTRY is ON
            // (inproc backend, curl transport). Only binaries that pull
            // CrashReporter.o need it — the MIB Studio app does.
            let sentry_dir = build_dir.join("_deps/sentry-build");
            if sentry_dir.join("libsentry.a").exists() {
                println!("cargo:rerun-if-changed={}/libsentry.a", sentry_dir.display());
                println!("cargo:rustc-link-search=native={}", sentry_dir.display());
                println!("cargo:rustc-link-lib=static=sentry");
            }
            // libcurl: sentry-native's transport and the backend's own
            // MinidumpUploader (CMake links CURL::libcurl when found).
            println!("cargo:rustc-link-lib=dylib=curl");
            println!("cargo:rustc-link-lib=dylib=sqlite3");
        }

        // System shared dependencies pulled in by the archives.
        let hdf5_dir = "/usr/lib/x86_64-linux-gnu/hdf5/serial";
        println!("cargo:rustc-link-search=native={hdf5_dir}");
        for lib in [
            "opencv_core",
            "opencv_imgproc",
            "opencv_imgcodecs",
            "opencv_videoio",
            "hdf5",
            "spdlog",
            "fmt",
            "crypto",
            "stdc++",
            "pthread",
            "dl",
            "z",
        ] {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
    }
}

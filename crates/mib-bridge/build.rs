// Build script for the Rust <-> C++ bridge (epic #246, ADR 0003).
//
// 1. Ensures the Qt-free static backend archives (libmib_backend.a,
//    libmib_processing.a) exist by driving the `linux-backend-only` CMake
//    preset (skippable with MIB_BRIDGE_NO_CMAKE=1 when the caller has already
//    built them, e.g. a CI job that ran cmake explicitly).
// 2. Compiles the cxx bridge + shim.cpp.
// 3. Links the static backend and its system dependencies (OpenCV / HDF5 /
//    SQLite / spdlog / fmt / crypto) — no Qt, no webkit, no display.
//
// Windows (MSVC/Conan tree): the same static libraries live in build/Release
// and their ~190 transitive import libraries are read from
// build/mib-bridge-link-manifest.json, written by
// tools/gen_bridge_link_manifest.py from the CMake-generated backend test
// project. MIB_BRIDGE_NO_CMAKE is implied: build the backend with the
// windows-default preset first.

use std::path::{Path, PathBuf};
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

#[cfg(not(windows))]
fn ensure_backend_built(repo: &Path, build_dir: &Path) {
    let backend_lib = build_dir.join("libmib_backend.a");
    let processing_lib = build_dir.join("libmib_processing.a");

    if std::env::var("MIB_BRIDGE_NO_CMAKE").is_ok() {
        if !backend_lib.exists() || !processing_lib.exists() {
            panic!(
                "MIB_BRIDGE_NO_CMAKE set but backend archives are missing in {}",
                build_dir.display()
            );
        }
        return;
    }

    // Configure (idempotent) then build only the two archives the bridge needs.
    let configure = Command::new("cmake")
        .current_dir(repo)
        .args(["--preset", "linux-backend-only"])
        .status();
    let built = Command::new("cmake")
        .current_dir(repo)
        .args([
            "--build",
            "--preset",
            "linux-backend-only-build",
            "--target",
            "mib_backend",
            "mib_processing",
        ])
        .status();

    let ok = matches!(configure, Ok(s) if s.success())
        && matches!(built, Ok(s) if s.success());

    if !ok {
        if backend_lib.exists() && processing_lib.exists() {
            println!(
                "cargo:warning=cmake backend build failed but archives exist; \
                 linking existing {}",
                build_dir.display()
            );
        } else {
            panic!("failed to build mib_backend/mib_processing via cmake preset");
        }
    }
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
             after building mib_backend, mib_processing and mib_backend_smoke_test): {e}",
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

    let mut bridge_build = cxx_build::bridge("src/lib.rs");
    if std::env::var_os("CARGO_FEATURE_CONTRACT_FIXTURES").is_some() {
        bridge_build.define("MIB_BRIDGE_CONTRACT_FIXTURES", None);
    }
    bridge_build
        .file("src/shim.cpp")
        .flag("/std:c++17")
        .flag("/EHsc")
        .flag("/utf-8")
        .flag("/Zc:__cplusplus").flag("/permissive-")
        .define("NOMINMAX", None)
        .define("WIN32_LEAN_AND_MEAN", None)
        .include(include_dir);
    for d in strings("include_dirs") {
        bridge_build.include(d);
    }
    for def in strings("defines") {
        match def.split_once('=') {
            Some((k, v)) => {
                bridge_build.define(k, v);
            }
            None => {
                bridge_build.define(&def, None);
            }
        }
    }
    bridge_build.compile("mib_bridge_shim");

    for d in strings("lib_dirs") {
        println!("cargo:rustc-link-search=native={d}");
    }
    // Order matters for static archives: the backend before its dependencies,
    // exactly as CMake linked the reference test.
    for lib in strings("libs") {
        if lib == "mib_backend" || lib == "mib_processing" {
            println!("cargo:rustc-link-lib=static={lib}");
        } else {
            println!("cargo:rustc-link-lib={lib}");
        }
    }
    for lib in ["mib_backend", "mib_processing"] {
        let dir = manifest["runtime_dirs"][0].as_str().unwrap_or("build/Release");
        println!("cargo:rerun-if-changed={dir}/{lib}.lib");
    }
}

fn main() {
    let repo = repo_root();
    let include_dir = repo.join("include");

    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_CONTRACT_FIXTURES");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=src/shim.cpp");
    println!("cargo:rerun-if-changed=src/shim.h");
    println!("cargo:rerun-if-env-changed=MIB_BRIDGE_NO_CMAKE");

    #[cfg(windows)]
    {
        windows_build(&repo, &include_dir);
        return;
    }

    #[cfg(not(windows))]
    {
        let build_dir = repo.join("build/linux-backend");
        ensure_backend_built(&repo, &build_dir);

        // Compile the cxx bridge + shim.
        let mut bridge_build = cxx_build::bridge("src/lib.rs");
        if std::env::var_os("CARGO_FEATURE_CONTRACT_FIXTURES").is_some() {
            bridge_build.define("MIB_BRIDGE_CONTRACT_FIXTURES", None);
        }
        bridge_build
            .file("src/shim.cpp")
            .flag_if_supported("-std=c++17")
            .include(&include_dir)
            .include("/usr/include/opencv4")
            .compile("mib_bridge_shim");

        // Relink when the backend archives change (e.g. a facade edit) so a stale
        // build dir can't silently keep an old symbol set.
        println!("cargo:rerun-if-changed={}/libmib_backend.a", build_dir.display());
        println!("cargo:rerun-if-changed={}/libmib_processing.a", build_dir.display());

        // Link the static backend archives (order matters: backend before processing).
        println!("cargo:rustc-link-search=native={}", build_dir.display());
        println!("cargo:rustc-link-lib=static=mib_backend");
        println!("cargo:rustc-link-lib=static=mib_processing");

        // System shared dependencies pulled in by the backend.
        let hdf5_dir = "/usr/lib/x86_64-linux-gnu/hdf5/serial";
        println!("cargo:rustc-link-search=native={hdf5_dir}");
        for lib in [
            "opencv_core",
            "opencv_imgproc",
            "opencv_imgcodecs",
            "opencv_videoio",
            "hdf5",
            "sqlite3",
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

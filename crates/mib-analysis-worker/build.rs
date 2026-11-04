fn main() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    cxx_build::bridge("src/native.rs")
        .file(root.join("src/backend/analysis/Population.cpp"))
        .file(root.join("src/backend/analysis/WorkerComposition.cpp"))
        .include(root.join("include"))
        .std("c++17")
        .compile("mib-analysis-native");
    for file in [
        "src/native.rs",
        "../../src/backend/analysis/Population.cpp",
        "../../src/backend/analysis/WorkerComposition.cpp",
        "../../include/backend/analysis/Population.h",
        "../../include/backend/analysis/WorkerComposition.h",
    ] {
        println!("cargo:rerun-if-changed={file}");
    }
}

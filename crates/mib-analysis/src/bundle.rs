use crate::{Error, Result};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{Read, Take};
use std::path::{Component, Path, PathBuf};

const MAX_FILES: usize = 20_000;
const MAX_BUNDLE_BYTES: u64 = 4 << 30;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Entry {
    sha256: String,
    size: u64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    schema: u32,
    distribution: String,
    toolkit_version: String,
    interpreter: String,
    helper: String,
    toolkit_wheel: String,
    files: BTreeMap<String, Entry>,
}

/// The expected digest comes from the application build, never from the bundle
/// itself or a webview. The installer must protect the directory from mutation.
pub struct VerifiedBundle {
    pub(crate) root: PathBuf,
    pub(crate) interpreter: PathBuf,
    pub(crate) helper: PathBuf,
    pub(crate) digest: String,
    pub(crate) wheel_digest: String,
    pub(crate) development: bool,
}

fn file_hash(mut input: Take<File>) -> Result<String> {
    let mut hash = Sha256::new();
    let mut buffer = [0; 64 * 1024];
    loop {
        let size = input.read(&mut buffer)?;
        if size == 0 {
            break;
        }
        hash.update(&buffer[..size]);
    }
    Ok(hex::encode(hash.finalize()))
}

fn relative(value: &str) -> Result<&Path> {
    let path = Path::new(value);
    if value.is_empty()
        || value.contains('\\')
        || value.contains(':')
        || value
            .split('/')
            .any(|p| p.is_empty() || p == "." || p == "..")
        || !path.components().all(|c| matches!(c, Component::Normal(_)))
    {
        return Err(Error::Bundle("invalid relative path"));
    }
    Ok(path)
}

fn inventory(
    root: &Path,
    dir: &Path,
    files: &mut BTreeSet<PathBuf>,
    count: &mut usize,
) -> Result<()> {
    if dir
        .strip_prefix(root)
        .map_err(|_| Error::Bundle("invalid root"))?
        .components()
        .count()
        > 32
    {
        return Err(Error::Bundle("bundle directory depth"));
    }
    for item in fs::read_dir(dir)? {
        let item = item?;
        *count += 1;
        if *count > MAX_FILES {
            return Err(Error::Bundle("too many bundle entries"));
        }
        let kind = item.file_type()?;
        if kind.is_symlink() {
            return Err(Error::Bundle("symlink in bundle"));
        }
        if kind.is_dir() {
            inventory(root, &item.path(), files, count)?;
        } else if kind.is_file() {
            let path = item.path();
            let path = path
                .strip_prefix(root)
                .map_err(|_| Error::Bundle("path escaped bundle"))?;
            if path != Path::new("manifest.json") {
                files.insert(path.to_path_buf());
            }
        } else {
            return Err(Error::Bundle("non-regular bundle entry"));
        }
    }
    Ok(())
}

impl VerifiedBundle {
    /// Blocking local-file verification; call before starting the async supervisor.
    /// `allow_development` must be false in production application code.
    pub fn load(root: &Path, expected_sha256: &str, allow_development: bool) -> Result<Self> {
        if fs::symlink_metadata(root)?.file_type().is_symlink() {
            return Err(Error::Bundle("bundle root is a symlink"));
        }
        let root = root.canonicalize()?;
        let path = root.join("manifest.json");
        let metadata = fs::symlink_metadata(&path)?;
        if !metadata.is_file() || metadata.len() > crate::MAX_MESSAGE as u64 {
            return Err(Error::Bundle("invalid manifest file"));
        }
        let mut bytes = Vec::new();
        File::open(path)?
            .take(crate::MAX_MESSAGE as u64 + 1)
            .read_to_end(&mut bytes)?;
        if bytes.len() > crate::MAX_MESSAGE
            || hex::encode(Sha256::digest(&bytes)) != expected_sha256
        {
            return Err(Error::Bundle("manifest digest mismatch"));
        }
        let manifest: Manifest = serde_json::from_slice(&bytes)?;
        let development = manifest.distribution == "development";
        // Abrupt-parent-exit containment is currently qualified only on Linux.
        // Windows production must wait for Job Object ownership at creation.
        #[cfg(not(target_os = "linux"))]
        if !development {
            return Err(Error::Bundle("production process ownership unavailable"));
        }
        if manifest.schema != 1
            || manifest.toolkit_version != "0.1.0"
            || (!development && manifest.distribution != "production")
            || (development && !allow_development)
        {
            return Err(Error::Bundle("unqualified manifest"));
        }
        let mut actual = BTreeSet::new();
        inventory(&root, &root, &mut actual, &mut 0)?;
        let mut expected = BTreeSet::new();
        let mut total = 0u64;
        for (name, entry) in &manifest.files {
            let relative = relative(name)?;
            expected.insert(relative.to_path_buf());
            total = total
                .checked_add(entry.size)
                .ok_or(Error::Bundle("bundle size overflow"))?;
            if total > MAX_BUNDLE_BYTES {
                return Err(Error::Bundle("bundle byte budget"));
            }
            let path = root.join(relative);
            let metadata = fs::symlink_metadata(&path)?;
            if !metadata.is_file()
                || metadata.len() != entry.size
                || file_hash(File::open(path)?.take(entry.size + 1))? != entry.sha256
            {
                return Err(Error::Bundle("bundle file digest mismatch"));
            }
        }
        if expected != actual {
            return Err(Error::Bundle("unlisted or missing bundle file"));
        }
        for name in [
            &manifest.interpreter,
            &manifest.helper,
            &manifest.toolkit_wheel,
        ] {
            relative(name)?;
            if !manifest.files.contains_key(name) {
                return Err(Error::Bundle("missing required file"));
            }
        }
        let wheel_digest = manifest.files[&manifest.toolkit_wheel].sha256.clone();
        Ok(Self {
            interpreter: root.join(manifest.interpreter),
            helper: root.join(manifest.helper),
            digest: expected_sha256.to_string(),
            wheel_digest,
            development,
            root,
        })
    }
}

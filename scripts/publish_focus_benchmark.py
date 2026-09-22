#!/usr/bin/env python3
"""Publish and verify a completed focus benchmark as a PRIVATE HF dataset.

Run through a supported protected HF_TOKEN environment. Never accepts a token
argument. No destructive replacement or public visibility changes are supported.
"""
import argparse
import hashlib
import json
from pathlib import Path


def validated_files(folder):
    manifest = json.loads((folder / "manifest.json").read_text())
    results = json.loads((folder / "benchmark/results.json").read_text())
    if manifest["status"] != "complete" or not manifest["total_retained"]:
        raise ValueError("dataset incomplete")
    digest = hashlib.sha256((folder / "manifest.json").read_bytes()).hexdigest()
    if results["dataset_manifest_sha256"] != digest:
        raise ValueError("benchmark and dataset manifest differ")
    if results["total_input_frames"] != manifest["total_retained"]:
        raise ValueError("benchmark frame accounting mismatch")
    paths = ["manifest.json", "README.md", "benchmark/results.json", "benchmark/README.md",
             "benchmark/metrics.csv", "benchmark/paired_objects.csv", "benchmark/per_frame.jsonl"]
    for recording in manifest["recordings"]:
        for key, hash_key in [("parquet", "parquet_sha256"), ("background", "background_png_sha256")]:
            relative = recording[key]
            path = (folder / relative).resolve()
            if not path.is_relative_to(folder.resolve()):
                raise ValueError("artifact escapes dataset directory")
            if hashlib.sha256(path.read_bytes()).hexdigest() != recording[hash_key]:
                raise ValueError("artifact hash mismatch")
            paths.append(relative)
    for relative in paths:
        if not (folder / relative).is_file(): raise ValueError(f"missing artifact: {relative}")
    return sorted(set(paths))


def publish(folder, repo, dry_run=False):
    paths = validated_files(folder)
    if dry_run:
        return dict(repo=repo, private=True, files=len(paths),
                    bytes=sum((folder/p).stat().st_size for p in paths))
    from huggingface_hub import HfApi, CommitOperationAdd
    from huggingface_hub.errors import RepositoryNotFoundError
    api = HfApi()
    try:
        info = api.repo_info(repo, repo_type="dataset")
    except RepositoryNotFoundError:
        api.create_repo(repo, repo_type="dataset", private=True)
        info = api.repo_info(repo, repo_type="dataset")
    if not info.private:
        raise ValueError("refusing to upload to a public repository")
    existing = set(api.list_repo_files(repo, repo_type="dataset")) - {".gitattributes"}
    if existing:
        raise ValueError("destination is not empty; refusing to overwrite existing data")
    commit = api.create_commit(repo, repo_type="dataset",
        operations=[CommitOperationAdd(path_in_repo=p, path_or_fileobj=str(folder/p)) for p in paths],
        commit_message="Add reproducible focus screening dataset and native core benchmark",
        parent_commit=info.sha)
    remote = api.repo_info(repo, repo_type="dataset", revision=commit.oid, files_metadata=True)
    files = {f.rfilename: f for f in remote.siblings}
    for relative in paths:
        if relative not in files or files[relative].size != (folder/relative).stat().st_size:
            raise ValueError("remote artifact inventory/size verification failed")
    return dict(repo=repo, private=remote.private, revision=commit.oid, files=len(paths),
                url=f"https://huggingface.co/datasets/{repo}/tree/{commit.oid}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    print(json.dumps(publish(args.dataset, args.repo, args.dry_run), indent=2))

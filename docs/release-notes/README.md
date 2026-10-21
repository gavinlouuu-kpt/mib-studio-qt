# Release notes

Create `vX.Y.Z.md` for every stable release. Curated notes are reviewed in the
release PR to main before building. Write for operators: say what they see and
where in the UI, with issue numbers in parentheses. Update the manual for every
user-visible change. Do not publish template placeholders. Highlights and Added
must contain text; use “None.” when a category has no changes.
Record SDK versions (including eGrabber and MindVision), required firmware and
upgrade steps explicitly. Beta notes are generated from recent vault fragments
and are labelled uncurated; they do not satisfy the stable gate.

```markdown
# MIB Studio vX.Y.Z — YYYY-MM-DD

## Highlights

TODO: Highlights

## Added

TODO: Added

## Changed

TODO: Changed

## Fixed

TODO: Fixed

## Removed

TODO: Removed

## Known issues

TODO: Known issues

## Hardware & compatibility

TODO: Hardware & compatibility

## Upgrade notes

TODO: Upgrade notes
```

## Pending notices for the next release

The next release of each product must carry these notices in its notes. Remove an entry
once a release carries it.

- **YOFO Review: notice for files from Regenerate masks (#737).** The next YOFO Review
  release (a `review-v*` tag or the shared `v*` release) must state, in its GitHub release
  notes and in the updater notes they link to:
  - **What was wrong:** releases up to and including review-v1.1.3-beta.1 wrote the wrong
    frame index, timestamp and track first/last frame into most records whenever processing
    produced fewer records than frames.
  - **What was not affected:** images, masks and background; MIB Studio's reanalysis.
  - **What to do:** regenerate affected files again with the new release.

  The user manual (`docs/manual/yofo-review.md`, Regenerate masks) already carries the
  notice. The release body in `.github/workflows/review-release.yml` is generic, so add the
  notice to the GitHub Release by hand when the release is cut.
- **YOFO Studio (browser UI): where files without a full path were saved (#762).** The
  next release with `yofo-studio-server` must state:
  - **What was wrong:** in the browser, a file name without a full path (the Start prompt's
    default `experiment.h5`, or a name typed into a save prompt for a recording, an export,
    Regenerate masks or a preview save) was written to the server's working directory, not
    to its data directory, so Files did not list it. The readiness check wrote its probe
    files there too.
  - **What changed:** such a path is now saved under the data directory, and the save
    prompt opens on `<data directory>/<name>` and says where the file goes. A path that
    would leave the data directory (`..`, or a link to elsewhere) is refused with the
    reason. Full paths are saved where they point, as before.
  - **What to do:** look for runs from earlier versions in the directory the server was
    started from (for a systemd unit, its `WorkingDirectory`, `/` by default) and move them
    into the data directory.
  - **Also:** while a file is open in Review, Start is still refused (HDF5 is not
    thread-safe); the message now says to close it in Review (Review → Close File).

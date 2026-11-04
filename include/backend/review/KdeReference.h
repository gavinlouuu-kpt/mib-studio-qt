#pragma once
// A reference core contour read from an experiment file (#866 PR E, bridge ABI 46 command
// `fetch_kde_reference_from_file`; Qt: ExperimentMonitoringTab::loadKdeReferenceFromFile). Read only:
// a short-lived Hdf5Service reader of its own opens the file, reads the stored KDE core record (the
// full-run analysis one first, the live one as the fallback), and closes it. It never opens the file
// in Review and writes nothing.
//
// HDF5 is built without thread safety (#722), so the caller serializes it as every other reader of
// an arbitrary file: ReviewJobs::readKdeReference holds the review lanes' lock and the session's
// mutex, and the facade only calls it inside the coordinator's idle transaction with no raw
// recording writing HDF5 (as the run quick look does).

#include "backend/processing/MonitoringDensity.h"

#include <cstddef>
#include <string>
#include <vector>

namespace backend::review {

// A stored contour larger than this is refused (the reply is JSON through the bridge). The
// live KDE's own grid (128 x 64) bounds what the service writes; this guards an arbitrary file.
inline constexpr std::size_t kKdeReferenceMaxVertices = 20000;

struct KdeReferenceResult {
    bool ok{false};
    // Why not, in plain words (Qt's wording): "file not found", "not a readable HDF5 file",
    // "stored core contour unreadable (...)", "the experiment has no stored core contour",
    // "the stored core contour is empty (too few cells)". Empty when ok.
    std::string reason;
    // "full-run" (the authoritative record, computed from the whole run) or "live" (the
    // provisional copy of the live Monitoring view): Qt's label word.
    std::string source;
    double coreFraction{0.0};
    std::vector<monitoring::Contour> contours; // closed loops, µm² x deformability
};

// Reads `path` (already resolved by the caller). Never throws.
KdeReferenceResult readKdeReference(const std::string& path);

// The bridge reply (bridge-contract.json fetch_kde_reference_from_file.reply):
// {ok, path, file_name, error, reason, source, core_fraction, core_percent, loops, contours}.
// `error` is "No reference contour loaded from <path>: <reason>" when not ok, else null.
std::string kdeReferenceReplyJson(const std::string& requestedPath, const KdeReferenceResult& result);

// The same refusal without a read (the facade's own reasons: a path outside the storage root, a
// run in progress, ...).
std::string kdeReferenceRefusedJson(const std::string& requestedPath, const std::string& reason);

} // namespace backend::review

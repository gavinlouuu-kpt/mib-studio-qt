// The E-modulus LUT a reanalysis, a regenerate or a one-record reprocess
// computes Young's modulus with (#764).
//
// A capture records the LUT it loaded in its run snapshot v3
// (`emodulus_lut`: loaded, lut id, revision, source, the sha256 of the loaded
// bytes). Reprocessing that run uses the same table, never whatever the host
// has loaded now:
//  - the source records a LUT: the table whose bytes have the recorded
//    sha256, found among the host kernel's loaded LUT and the bundled LUT
//    (compiled into the review core, so a shell without the resources folder
//    has it). Not found, or no hash recorded: refused with the reason;
//  - the source records that no LUT was loaded: no E (E stays 0, E gates
//    cannot pass, reported as not computed), as the source;
//  - the source records nothing (no snapshot, or one before v3; folder and
//    AVI sources): the host's behaviour before #764 (Legacy), which the
//    caller says in its reply.
#pragma once

#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/RunProvenanceRecord.h"

#include <memory>
#include <optional>
#include <string>

namespace backend::services
{
    class Hdf5Service;
}

namespace backend::review
{

    enum class EModulusLutBasis
    {
        Legacy,    // the source records no E-LUT: the host's own, as before #764
        Source,    // the source run's own table, found by its recorded hash
        NotLoaded, // the source run loaded none: no E, as the source
    };

    struct SourceEModulusLut
    {
        EModulusLutBasis basis{EModulusLutBasis::Legacy};
        std::shared_ptr<const EModulusLut> table; // Source only
        // Source: the recorded LUT in words, e.g.
        // "scaled_isoelastic_data_LUT_6.16-4.24 bundled (sha256 8a86b2beef4f)".
        std::string name;
        std::string sha256; // Source: the recorded hash, lowercase
        // Sets the call's table: Source this one, NotLoaded none; Legacy
        // leaves the service's own.
        void applyTo(services::ProcessingService::BatchAnalysisInputs &inputs) const;
    };

    // The bundled LUT (resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt)
    // compiled into the review core, and the sha256 of its bytes.
    const std::string &bundledEModulusLutText();
    const std::string &bundledEModulusLutSha256();

    // A recorded LUT in words: lut id, revision and the first 12 hex digits
    // of its hash, the parts recorded ("an unidentified E-LUT" with none).
    std::string describeEModulusLut(const recording::EModulusLutRecord &record);
    // The host kernel's loaded LUT in the same words; "" when none is loaded.
    std::string describeLoadedEModulusLut(const services::ProcessingService &host);

    // The open file's recorded LUT (snapshot v3 `emodulus_lut`); nullopt
    // when the file has no snapshot, an unreadable one, or one without it.
    std::optional<recording::EModulusLutRecord> recordedEModulusLut(const services::Hdf5Service &reader);

    // Resolves `record` (see the file comment). `host` (may be null) is the
    // kernel whose loaded LUT is a candidate. False, with a plain `error`,
    // when the source records a LUT that cannot be identified or found here.
    // Reads no file; safe from any thread.
    bool resolveSourceEModulusLut(const std::optional<recording::EModulusLutRecord> &record,
                                  const services::ProcessingService *host, SourceEModulusLut &out,
                                  std::string &error);

} // namespace backend::review

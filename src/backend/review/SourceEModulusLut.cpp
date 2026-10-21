// The source run's own E-LUT for reanalysis, regenerate and reprocess (#764);
// see SourceEModulusLut.h.
#include "backend/review/SourceEModulusLut.h"

#include "backend/processing/ProcessingCoreSha256.h"
#include "backend/recording/Hdf5Service.h"

#include "BundledEModulusLut.h"

#include <algorithm>
#include <cctype>

namespace backend::review
{

    namespace
    {
        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string words(const std::string &lutId, const std::string &revision, const std::string &sha256)
        {
            std::string out = lutId;
            if (!revision.empty()) out += (out.empty() ? "" : " ") + revision;
            if (!sha256.empty())
                out += (out.empty() ? "" : " ") + std::string("(sha256 ") + lower(sha256).substr(0, 12) + ")";
            return out;
        }

        std::string shortHash(const std::string &sha256) { return sha256.empty() ? "none" : lower(sha256).substr(0, 12); }

        // Built once: the grid build scans every point per cell.
        std::shared_ptr<const EModulusLut> bundledTable()
        {
            static const std::shared_ptr<const EModulusLut> table = [] {
                auto lut = std::make_shared<EModulusLut>();
                if (!lut->loadFromText(bundledEModulusLutText(), "the bundled E-LUT (compiled in)")) return std::shared_ptr<const EModulusLut>{};
                return std::shared_ptr<const EModulusLut>(std::move(lut));
            }();
            return table;
        }
    } // namespace

    void SourceEModulusLut::applyTo(services::ProcessingService::BatchAnalysisInputs &inputs) const
    {
        if (basis == EModulusLutBasis::Source) inputs.eModulusLut = table.get();
        else if (basis == EModulusLutBasis::NotLoaded) inputs.eModulusLut = nullptr;
    }

    const std::string &bundledEModulusLutText()
    {
        static const std::string text = generatedBundledEModulusLutText();
        return text;
    }

    const std::string &bundledEModulusLutSha256()
    {
        static const std::string sha = lower(processing::processingCoreBytesSha256(
            reinterpret_cast<const std::uint8_t *>(bundledEModulusLutText().data()), bundledEModulusLutText().size()));
        return sha;
    }

    std::string describeEModulusLut(const recording::EModulusLutRecord &record)
    {
        const std::string out =
            words(record.lutId.value_or(""), record.revision.value_or(""), record.sha256.value_or(""));
        return out.empty() ? "an unidentified E-LUT" : out;
    }

    std::string describeLoadedEModulusLut(const services::ProcessingService &host)
    {
        if (!host.eModulusLut().isLoaded()) return {};
        const auto identity = host.eModulusLutIdentity();
        const std::string out = words(identity.lutId, identity.revision, identity.sha256);
        return out.empty() ? "an unidentified E-LUT" : out;
    }

    std::optional<recording::EModulusLutRecord> recordedEModulusLut(const services::Hdf5Service &reader)
    {
        recording::RecordedRunProvenance provenance;
        if (!recording::readRecordedRunProvenance(reader, provenance)) return std::nullopt;
        return provenance.v3.eModulusLut;
    }

    bool resolveSourceEModulusLut(const std::optional<recording::EModulusLutRecord> &record,
                                  const services::ProcessingService *host, SourceEModulusLut &out, std::string &error)
    {
        out = SourceEModulusLut{};
        if (!record) return true; // Legacy: the host's own, as before
        if (!record->loaded)
        {
            out.basis = EModulusLutBasis::NotLoaded;
            return true;
        }
        const std::string name = describeEModulusLut(*record);
        const std::string want = lower(record->sha256.value_or(""));
        if (want.empty())
        {
            error = "E cannot be recomputed with the source run's E-LUT (" + name +
                    "): the run records no hash to identify it by";
            return false;
        }
        std::string hostHash;
        if (host && host->eModulusLut().isLoaded())
        {
            const auto identity = host->eModulusLutIdentity();
            hostHash = lower(identity.sha256);
            if (identity.loaded && hostHash == want)
            {
                // A copy: the kernel's table stays the kernel's.
                auto copy = std::make_shared<EModulusLut>(host->eModulusLut());
                if (copy->isLoaded()) out.table = std::move(copy);
            }
        }
        if (!out.table && bundledEModulusLutSha256() == want) out.table = bundledTable();
        if (!out.table)
        {
            error = "E cannot be recomputed with the source run's E-LUT " + name +
                    ": no E-LUT with that hash is available here (loaded: " + shortHash(hostHash) +
                    "; bundled: " + shortHash(bundledEModulusLutSha256()) + ")";
            return false;
        }
        out.basis = EModulusLutBasis::Source;
        out.name = name;
        out.sha256 = want;
        return true;
    }

} // namespace backend::review

#include "backend/analysis/AnalysisLibrary.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
namespace {
void fields(const Json& value, const std::set<std::string>& names) {
    if (!value.is_object() || value.size() != names.size())
        throw std::invalid_argument("Incomplete library fields");
    for (auto i = value.begin(); i != value.end(); ++i)
        if (!names.count(i.key())) throw std::invalid_argument("Unknown library field");
}
std::string text(const Json& value, std::size_t limit) {
    const auto s = value.get<std::string>();
    if (s.empty() || s.size() > limit) throw std::invalid_argument("Invalid library label budget");
    return s;
}
void labels(const Json& values, std::size_t limit) {
    if (!values.is_array() || values.size() > limit)
        throw std::invalid_argument("Library annotation budget exceeded");
    std::set<std::string> seen;
    for (const auto& value : values)
        if (!seen.insert(text(value, 128)).second)
            throw std::invalid_argument("Duplicate library annotation");
}
void preferences(const Json& value) {
    if (!value.is_object()) throw std::invalid_argument("Expected display preferences");
    for (auto i = value.begin(); i != value.end(); ++i) {
        if (i.key() == "decimalDigits") {
            if (!i->is_number_integer() || i->get<int>() < 2 || i->get<int>() > 8)
                throw std::invalid_argument("Display precision must be 2..8 digits");
        } else if (i.key() == "showUnknown") {
            if (!i->is_boolean()) throw std::invalid_argument("Expected showUnknown boolean");
        } else
            throw std::invalid_argument(
                "Unsupported display preference; scientific settings are not preferences");
    }
}
Json effective(const Json& library, const Json& sample) {
    Json result{{"decimalDigits", 4}, {"showUnknown", true}},
        origin{{"decimalDigits", "application-default"}, {"showUnknown", "application-default"}};
    for (const auto& item :
         std::vector<std::pair<std::string, Json>>{{"library", library}, {"sample", sample}})
        for (auto i = item.second.begin(); i != item.second.end(); ++i) {
            result[i.key()] = i.value();
            origin[i.key()] = item.first;
        }
    return Json{{"values", result}, {"origin", origin}};
}
} // namespace
Json analysisLibraryRequest(services::SqliteService& db, const Json& q, bool write,
                            const std::function<Json(const Json&)>& source) {
    const auto action = q.at("action").get<std::string>();
    const auto id = text(q.at("libraryId"), 128);
    const auto store = [&](const Json& request, bool mutate = false) {
        return Json::parse(db.analysisWorkspaceRequest(request.dump(), mutate));
    };
    if (action == "libraryRead") {
        fields(q, {"action", "libraryId", "revision"});
        if (!q.at("revision").is_string() || q.at("revision").get<std::string>().empty())
            throw std::invalid_argument("Exact library revision required");
        auto saved = store(Json{{"action", "read"},
                                {"workspace_id", "analysis-library/" + id},
                                {"revision", q.at("revision")}});
        if (saved.value("status", std::string{}) != "ok") return saved;
        const auto& doc = saved.at("document");
        if (doc.at("type") != "F-sample-library/1" ||
            contentHash(doc.at("library")) != doc.at("contentHash"))
            throw std::invalid_argument("Library integrity mismatch");
        Json display = Json::array();
        for (const auto& sample : doc.at("library").at("samples"))
            display.push_back(Json{{"fingerprint", sample.at("binding").at("fingerprint")},
                                   {"preferences", effective(doc.at("library").at("preferences"),
                                                             sample.at("preferences"))}});
        saved["display"] = display;
        return saved;
    }
    if (action != "librarySave" || !write)
        throw std::invalid_argument("Library save requires write endpoint");
    fields(q, {"action", "libraryId", "expectedRevision", "commandId", "library"});
    const auto& draft = q.at("library");
    fields(draft, {"title", "groups", "preferences", "samples"});
    text(draft.at("title"), 256);
    labels(draft.at("groups"), 64);
    preferences(draft.at("preferences"));
    if (!draft.at("samples").is_array() || draft.at("samples").size() > 128)
        throw std::invalid_argument("Library supports at most 128 retained sources");
    std::set<std::string> groups, fingerprints;
    for (const auto& g : draft.at("groups"))
        groups.insert(g.get<std::string>());
    auto library = draft;
    for (auto& sample : library.at("samples")) {
        fields(sample, {"source", "label", "groups", "tags", "notes", "preferences"});
        text(sample.at("label"), 256);
        labels(sample.at("groups"), 64);
        labels(sample.at("tags"), 32);
        preferences(sample.at("preferences"));
        if (!sample.at("notes").is_string() || sample.at("notes").get<std::string>().size() > 4096)
            throw std::invalid_argument("Library note budget exceeded");
        for (const auto& g : sample.at("groups"))
            if (!groups.count(g.get<std::string>()))
                throw std::invalid_argument("Sample refers to missing group");
        if (sample.at("source").at("kind") != "retained")
            throw std::invalid_argument("Library annotations require retained immutable sources");
        const auto snapshot = source(sample.at("source"));
        if (!fingerprints.insert(snapshot.at("fingerprint").get<std::string>()).second)
            throw std::invalid_argument("Duplicate source fingerprint in library");
        Json binding;
        for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"})
            binding[k] = snapshot.at(k);
        sample["binding"] = binding;
    }
    const Json doc{{"schema_version", 1},
                   {"type", "F-sample-library/1"},
                   {"library", library},
                   {"contentHash", contentHash(library)}};
    return store(Json{{"action", "commit"},
                      {"workspace_id", "analysis-library/" + id},
                      {"expected_revision", q.at("expectedRevision")},
                      {"command_id", q.at("commandId")},
                      {"document", doc}},
                 true);
}
} // namespace backend::analysis

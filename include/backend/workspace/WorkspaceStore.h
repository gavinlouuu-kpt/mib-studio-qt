#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;
namespace backend::workspace {
enum class StoreErrorCode { Invalid, Conflict, NotFound, Storage };
class StoreError : public std::runtime_error {
public:
    StoreError(StoreErrorCode code, const std::string& message)
        : std::runtime_error(message), code(code) {}
    StoreErrorCode code;
};
struct Snapshot {
    std::string workspaceId;
    std::string revision; // Canonical decimal string, never a JSON floating-point number.
    std::string documentJson;
};
// Connection and operator authorization belong to the metadata DB host (#393/#395).
// Caller serializes access; never use on acquisition threads or inside another transaction.
// Does not open files, own a DB, configure pragmas, apply hardware, or evaluate science.
class WorkspaceStore {
public:
    explicit WorkspaceStore(sqlite3* connection);
    void initialize();
    std::optional<Snapshot> read(const std::string& workspaceId,
                                 const std::optional<std::string>& revision = std::nullopt) const;
    std::vector<std::string> history(const std::string& workspaceId, int limit = 100,
                                     const std::optional<std::string>& after = std::nullopt) const;
    // Ordered metadata-only heads; caller supplies a literal namespace prefix.
    std::vector<Snapshot> catalog(const std::string& prefix, int limit, const std::string& after) const;
    // null expectedRevision is create-only. Complete logical document is atomically replaced.
    // Successful command replay returns its original revision even if the head advanced.
    Snapshot commit(const std::string& workspaceId,
                    const std::optional<std::string>& expectedRevision,
                    const std::string& commandId, const std::string& documentJson);
    std::optional<Snapshot> receipt(const std::string& workspaceId,
        const std::optional<std::string>& expectedRevision, const std::string& commandId,
        const std::string& documentJson) const;
    Snapshot restore(const std::string& workspaceId, const std::string& expectedRevision,
                     const std::string& commandId, const std::string& targetRevision);

private:
    sqlite3* db_;
    bool initialized_{false};
    void requireInitialized() const;
    Snapshot write(const std::string&, const std::optional<std::string>&, const std::string&,
                   const std::string&, const std::string&);
};
} // namespace backend::workspace

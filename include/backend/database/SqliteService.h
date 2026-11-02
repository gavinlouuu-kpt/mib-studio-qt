#pragma once
#include <memory>
#include <mutex>
#include <string>
struct sqlite3;
namespace backend::workspace {
class WorkspaceStore;
}
namespace backend::services {
// Owns the app metadata connection. All operations serialize on mutex_, never on capture threads.
class SqliteService {
public:
    SqliteService();
    ~SqliteService();
    bool initialize(const std::string& dbPath);
    bool execute(const std::string& sql);
    std::string workspaceRequest(const std::string& request, bool write);
    // Native analysis host only; never expose this as a generic workspace RPC.
    std::string analysisWorkspaceRequest(const std::string& request, bool write);
    // Native recipe provider only (#807 I02): read, catalog and create-only commit in the
    // reserved "recipe/" namespace. No other caller may write there.
    std::string recipeStoreRequest(const std::string& request, bool write);

private:
    enum class Authority { Public, Analysis, Recipe };
    std::string request(const std::string& request, bool write, Authority authority);
    std::mutex mutex_;
    sqlite3* db_{nullptr};
    std::unique_ptr<workspace::WorkspaceStore> workspace_;
};
} // namespace backend::services

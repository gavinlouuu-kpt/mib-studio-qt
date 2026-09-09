// Verifies the CrashReporter pending-dump upload queue on disk.
//
// The queue is advanced only on delivery evidence (MinidumpUploader's HTTP
// status), never on a hand-off assumption:
//   - .dmp → .dmp.sent on HTTP 2xx (sidecar and .txt note follow).
//   - .dmp → .dmp.rejected on a permanent 4xx (the server will never take it).
//   - .dmp stays .dmp when the server is unreachable, times out, or answers
//     5xx/429 — retried on the next launch, oldest first, at most
//     maxUploadsPerStart per launch.
//   - No DSN → nothing is uploaded or renamed.
//   - Legacy .dmp.uploaded / .dmp.queued / .dmp.queued2 (earlier optimistic
//     schemes) go back to .dmp and re-enter the queue.
//   - Bounded retention per class (delivered, pending, orphan sidecars/notes).
//
// A tiny local HTTP server plays Sentry's minidump endpoint so the checks
// are deterministic and offline. Checks use an explicit failure counter (not
// assert) so the test still verifies behavior in Release/NDEBUG builds.

#include "backend/services/CrashReporter.h"
#include "backend/services/MinidumpUploader.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
static void closeSocket(socket_t s) { ::closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
static void closeSocket(socket_t s) { ::close(s); }
#endif

namespace fs = std::filesystem;
using CrashReporter = backend::services::CrashReporter;
namespace crash_upload = backend::services::crash_upload;

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "     \
                      << #cond << "\n";                                     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static void createFile(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    CHECK(f.is_open());
    f << content;
}

static bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

static fs::path makeTempDir(const std::string& label) {
    auto dir = fs::temp_directory_path() / ("crash_reporter_test_" + label);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

static void cleanDir(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ── Minimal HTTP server standing in for /api/<project>/minidump/ ──────
// Single-threaded accept loop; answers every POST with the configured status
// and records the request bodies.
class FakeSentry {
public:
    explicit FakeSentry(int status) : status_(status) {
#ifdef _WIN32
        WSADATA wsa;
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listener_, 8);
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeSentry() {
        stop_.store(true);
        // Unblock accept() with a throwaway connection.
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closeSocket(s);
        if (thread_.joinable()) thread_.join();
        closeSocket(listener_);
#ifdef _WIN32
        ::WSACleanup();
#endif
    }

    std::string dsn() const {
        return "http://testkey@127.0.0.1:" + std::to_string(port_) + "/42";
    }

    std::vector<std::string> requests() {
        std::scoped_lock lock(mutex_);
        return requests_;
    }

private:
    void serve() {
        while (!stop_.load()) {
            socket_t client = ::accept(listener_, nullptr, nullptr);
            if (client == kInvalidSocket) break;
            if (stop_.load()) {
                closeSocket(client);
                break;
            }
            std::string data;
            char buf[8192];
            size_t contentLength = 0;
            size_t headerEnd = std::string::npos;
            for (;;) {
                const int n = ::recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                data.append(buf, static_cast<size_t>(n));
                if (headerEnd == std::string::npos) {
                    headerEnd = data.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const auto cl = data.find("Content-Length:");
                        if (cl != std::string::npos && cl < headerEnd) {
                            contentLength = static_cast<size_t>(
                                std::strtoull(data.c_str() + cl + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    data.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            {
                std::scoped_lock lock(mutex_);
                requests_.push_back(data);
            }
            const std::string reason = status_ == 200 ? "OK" : status_ == 400 ? "Bad Request" : "Error";
            const std::string body = status_ == 200 ? "{\"id\":\"deadbeef\"}" : "{}";
            const std::string response = "HTTP/1.1 " + std::to_string(status_) + " " + reason +
                                         "\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
            ::send(client, response.c_str(), static_cast<int>(response.size()), 0);
            closeSocket(client);
        }
    }

    int status_;
    socket_t listener_{kInvalidSocket};
    unsigned short port_{0};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

// Waits until `count` dumps in `dir` carry a terminal suffix (.sent or
// .rejected) — the uploader runs on its own thread and shutdown() only
// guarantees the dump in flight; a real session gives it time, so do we.
static bool waitForDelivered(const fs::path& dir, size_t count, int timeoutMs = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        size_t n = 0;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string fn = entry.path().filename().string();
            if (fn.find(".dmp.sent") != std::string::npos || fn.find(".dmp.rejected") != std::string::npos) ++n;
        }
        if (n >= count) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

static CrashReporter::Config baseConfig(const fs::path& dir, const std::string& dsn) {
    CrashReporter::Config cfg;
    cfg.crashDir = dir;
    cfg.databaseDir = dir / "sentry-db";
    cfg.uploadPendingOnStart = true;
    cfg.installSignalHandlers = false;
    cfg.installTerminateHandler = false;
    cfg.dsn = dsn;
    cfg.uploadTimeoutMs = 10000;
    return cfg;
}

// A DSN whose host refuses connections immediately: port 1 on loopback.
static const char* kUnreachableDsn = "http://testkey@127.0.0.1:1/42";

// ── Test 0: DSN parsing and request shape ─────────────────────────
static void testDsnAndMultipart() {
    std::cout << "  test: DSN parsing + multipart body ... ";
    auto d = crash_upload::parseDsn("https://1ea057e5426c0ca133c34ba142780750@sentry.yofo.bio/2");
    CHECK(d.valid);
    CHECK(d.scheme == "https" && d.host == "sentry.yofo.bio" && d.port == 0);
    CHECK(d.publicKey == "1ea057e5426c0ca133c34ba142780750" && d.projectId == "2");
    CHECK(crash_upload::minidumpUrl(d) ==
          "https://sentry.yofo.bio/api/2/minidump/?sentry_key=1ea057e5426c0ca133c34ba142780750");

    d = crash_upload::parseDsn("http://k:secret@10.0.0.5:9000/prefix/7");
    CHECK(d.valid && d.port == 9000 && d.basePath == "/prefix" && d.projectId == "7");
    CHECK(crash_upload::minidumpUrl(d) == "http://10.0.0.5:9000/prefix/api/7/minidump/?sentry_key=k");

    CHECK(!crash_upload::parseDsn("").valid);
    CHECK(!crash_upload::parseDsn("sentry.yofo.bio/2").valid);
    CHECK(!crash_upload::parseDsn("https://@host/2").valid);
    CHECK(!crash_upload::parseDsn("https://key@host").valid);

    crash_upload::UploadFields f;
    f.release = "mib_studio_qt@1.0.8";
    f.environment = "production";
    f.buildId = "ABC1";
    f.dumpFileName = "20260908T101931-pid3532-sigsegv.dmp";
    f.crashMessage = "what(): \"boom\"";
    f.stateSnapshotJson = "{\"capture\":\"running\"}";
    const auto body = crash_upload::buildMultipart("MDMP\x00\x01", f, "BOUNDARY");
    CHECK(body.contentType == "multipart/form-data; boundary=BOUNDARY");
    CHECK(body.body.find("name=\"sentry\"") != std::string::npos);
    CHECK(body.body.find("\"release\":\"mib_studio_qt@1.0.8\"") != std::string::npos);
    CHECK(body.body.find("\"exe_build_id\":\"ABC1\"") != std::string::npos);
    CHECK(body.body.find("\"crash_message\":\"what(): \\\"boom\\\"\"") != std::string::npos);
    CHECK(body.body.find("name=\"upload_file_minidump\"; filename=\"20260908T101931-pid3532-sigsegv.dmp\"") !=
          std::string::npos);
    CHECK(body.body.find("filename=\"state_snapshot.json\"") != std::string::npos);
    CHECK(body.body.find("--BOUNDARY--\r\n") == body.body.size() - 14);
    std::cout << "done\n";
}

// ── Test 1: legacy states are recovered and re-queued ─────────────
static void testLegacyRecovery() {
    std::cout << "  test: legacy .uploaded/.queued/.queued2 recovery ... ";
    auto dir = makeTempDir("legacy");

    createFile(dir / "20260101T120000-pid1-sigsegv.dmp.uploaded", "MDMP legacy uploaded");
    createFile(dir / "20260101T120000-pid1-sigsegv.json.uploaded", R"({"capture":"stopped"})");
    createFile(dir / "20260102T120000-pid2-sigsegv.dmp.queued", "MDMP legacy queued");
    createFile(dir / "20260102T120000-pid2-sigsegv.json.queued", R"({"q":1})");
    createFile(dir / "20260103T120000-pid3-sigsegv.dmp.queued2", "MDMP legacy queued2");
    createFile(dir / "20260103T120000-pid3-sigsegv.json.queued2", R"({"q":2})");
    // A sidecar with no dump stays untouched.
    createFile(dir / "20260202T010000-pid9999-seh.json.uploaded", R"({"orphan":true})");

    // No DSN: recovery happens, nothing is uploaded.
    CrashReporter::init(baseConfig(dir, ""));
    CrashReporter::shutdown();

    for (const char* base : {"20260101T120000-pid1-sigsegv", "20260102T120000-pid2-sigsegv",
                             "20260103T120000-pid3-sigsegv"}) {
        const std::string b = base;
        CHECK(fileExists(dir / (b + ".dmp")));
        CHECK(fileExists(dir / (b + ".json")));
        CHECK(!fileExists(dir / (b + ".dmp.uploaded")));
        CHECK(!fileExists(dir / (b + ".dmp.queued")));
        CHECK(!fileExists(dir / (b + ".dmp.queued2")));
        CHECK(!fileExists(dir / (b + ".dmp.sent")));
    }
    CHECK(fileExists(dir / "20260202T010000-pid9999-seh.json.uploaded"));

    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 2: delivery moves the queue; the request carries the dump ─
static void testDelivered() {
    std::cout << "  test: HTTP 200 → .sent with sidecar and note ... ";
    auto dir = makeTempDir("delivered");
    FakeSentry server(200);

    createFile(dir / "20260301T080000-pid100-sigsegv.dmp", "MDMP first dump bytes");
    createFile(dir / "20260301T080000-pid100-sigsegv.json", R"({"capture":"running","frames":42})");
    createFile(dir / "20260302T090000-pid200-terminate.dmp", "MDMP second dump");
    createFile(dir / "20260302T090000-pid200-terminate.txt", "what(): disk full");
    // Second dump has no sidecar — verifies isolation.

    CrashReporter::init(baseConfig(dir, server.dsn()));
    CHECK(waitForDelivered(dir, 2));
    CrashReporter::shutdown();  // joins the uploader

    CHECK(fileExists(dir / "20260301T080000-pid100-sigsegv.dmp.sent"));
    CHECK(fileExists(dir / "20260301T080000-pid100-sigsegv.json.sent"));
    CHECK(!fileExists(dir / "20260301T080000-pid100-sigsegv.dmp"));
    CHECK(!fileExists(dir / "20260301T080000-pid100-sigsegv.json"));
    CHECK(fileExists(dir / "20260302T090000-pid200-terminate.dmp.sent"));
    CHECK(fileExists(dir / "20260302T090000-pid200-terminate.txt.sent"));
    CHECK(!fileExists(dir / "20260302T090000-pid200-terminate.json.sent"));

    const auto requests = server.requests();
    CHECK(requests.size() == 2);
    if (requests.size() == 2) {
        // Oldest first (mtime order == creation order here).
        CHECK(requests[0].find("POST /api/42/minidump/?sentry_key=testkey HTTP/1.1") == 0);
        CHECK(requests[0].find("MDMP first dump bytes") != std::string::npos);
        CHECK(requests[0].find("\"frames\":42") != std::string::npos);
        CHECK(requests[0].find("\"original_dump_file\":\"20260301T080000-pid100-sigsegv.dmp\"") !=
              std::string::npos);
        CHECK(requests[1].find("\"crash_message\":\"what(): disk full\"") != std::string::npos);
        CHECK(requests[1].find("filename=\"state_snapshot.json\"") == std::string::npos);
    }

    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 3: permanent refusal is terminal, server errors are not ──
static void testRejectedAndServerError() {
    std::cout << "  test: HTTP 400 → .rejected, HTTP 503 → stays .dmp ... ";
    auto dir = makeTempDir("rejected");
    {
        FakeSentry server(400);
        createFile(dir / "20260401T080000-pid1-sigsegv.dmp", "MDMP too big for the server");
        createFile(dir / "20260401T080000-pid1-sigsegv.json", "{}");
        CrashReporter::init(baseConfig(dir, server.dsn()));
        CHECK(waitForDelivered(dir, 1));
        CrashReporter::shutdown();
        CHECK(fileExists(dir / "20260401T080000-pid1-sigsegv.dmp.rejected"));
        CHECK(fileExists(dir / "20260401T080000-pid1-sigsegv.json.rejected"));
        CHECK(!fileExists(dir / "20260401T080000-pid1-sigsegv.dmp"));
    }
    {
        FakeSentry server(503);
        createFile(dir / "20260402T080000-pid2-sigsegv.dmp", "MDMP server down");
        CrashReporter::init(baseConfig(dir, server.dsn()));
        CrashReporter::shutdown();
        CHECK(fileExists(dir / "20260402T080000-pid2-sigsegv.dmp"));
        CHECK(!fileExists(dir / "20260402T080000-pid2-sigsegv.dmp.sent"));
        CHECK(!fileExists(dir / "20260402T080000-pid2-sigsegv.dmp.rejected"));
        // The .rejected one from before was not retried.
        CHECK(server.requests().size() == 1);
    }
    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 4: unreachable server leaves everything pending ──────────
static void testUnreachable() {
    std::cout << "  test: unreachable server → dumps stay pending ... ";
    auto dir = makeTempDir("unreachable");
    createFile(dir / "20260501T080000-pid1-sigsegv.dmp", "MDMP offline");
    createFile(dir / "20260501T080000-pid1-sigsegv.json", "{}");
    createFile(dir / "20260502T080000-pid2-sigsegv.dmp", "MDMP offline 2");

    auto cfg = baseConfig(dir, kUnreachableDsn);
    cfg.uploadTimeoutMs = 5000;
    const auto t0 = std::chrono::steady_clock::now();
    CrashReporter::init(cfg);
    CrashReporter::shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(fileExists(dir / "20260501T080000-pid1-sigsegv.dmp"));
    CHECK(fileExists(dir / "20260501T080000-pid1-sigsegv.json"));
    CHECK(fileExists(dir / "20260502T080000-pid2-sigsegv.dmp"));
    // A refused connection must not burn the per-dump timeout twice: the
    // loop stops at the first transport failure.
    CHECK(elapsed < std::chrono::seconds(8));

    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 5: per-launch cap, oldest first ──────────────────────────
static void testPerLaunchCap() {
    std::cout << "  test: maxUploadsPerStart takes the oldest dumps ... ";
    auto dir = makeTempDir("cap");
    FakeSentry server(200);
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 5; ++i) {
        const std::string base = "2026060" + std::to_string(i + 1) + "T120000-pid" + std::to_string(i) + "-sigsegv";
        createFile(dir / (base + ".dmp"), "MDMP " + std::to_string(i));
        fs::last_write_time(dir / (base + ".dmp"), now - std::chrono::hours(24) * (10 - i), ec);
    }
    auto cfg = baseConfig(dir, server.dsn());
    cfg.maxUploadsPerStart = 2;
    CrashReporter::init(cfg);
    CHECK(waitForDelivered(dir, 2));
    CrashReporter::shutdown();

    CHECK(fileExists(dir / "20260601T120000-pid0-sigsegv.dmp.sent"));
    CHECK(fileExists(dir / "20260602T120000-pid1-sigsegv.dmp.sent"));
    CHECK(fileExists(dir / "20260603T120000-pid2-sigsegv.dmp"));
    CHECK(fileExists(dir / "20260605T120000-pid4-sigsegv.dmp"));
    CHECK(server.requests().size() == 2);

    // Next launch takes the next two.
    CrashReporter::init(cfg);
    CHECK(waitForDelivered(dir, 4));
    CrashReporter::shutdown();
    CHECK(fileExists(dir / "20260603T120000-pid2-sigsegv.dmp.sent"));
    CHECK(fileExists(dir / "20260604T120000-pid3-sigsegv.dmp.sent"));
    CHECK(fileExists(dir / "20260605T120000-pid4-sigsegv.dmp"));

    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 6: bounded retention of delivered dumps ──────────────────
static void testBoundedRetention() {
    std::cout << "  test: bounded retention of .sent/.rejected ... ";
    auto dir = makeTempDir("retention");
    for (int i = 0; i < 5; ++i) {
        const std::string name = "2026030" + std::to_string(i + 1) + "T120000-pid" + std::to_string(i) +
                                 (i % 2 ? "-sigsegv.dmp.rejected" : "-sigsegv.dmp.sent");
        createFile(dir / name, "MDMP delivered " + std::to_string(i));
    }
    auto cfg = baseConfig(dir, "");
    cfg.maxRetainedDumps = 3;
    CrashReporter::init(cfg);
    CrashReporter::shutdown();

    int remaining = 0;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.find(".dmp.sent") != std::string::npos || fn.find(".dmp.rejected") != std::string::npos) ++remaining;
    }
    CHECK(remaining == 3);
    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 7: orphan sidecar retention ──────────────────────────────
static void testOrphanSidecarRetention() {
    std::cout << "  test: orphan .json sidecar retention ... ";
    auto dir = makeTempDir("orphan_json");
    for (int i = 0; i < 5; ++i) {
        createFile(dir / ("2026040" + std::to_string(i + 1) + "T120000-pid" + std::to_string(i) + "-terminate.json"),
                   R"({"reason":"terminate"})");
    }
    for (int i = 0; i < 5; ++i) {
        createFile(dir / ("2026040" + std::to_string(i + 1) + "T130000-pid" + std::to_string(i) + "-terminate.txt"),
                   "terminate: stray note");
    }
    createFile(dir / "20260410T120000-pid9-sigsegv.dmp", "MDMP pending");
    createFile(dir / "20260410T120000-pid9-sigsegv.json", R"({"snapshot":"pending"})");
    createFile(dir / "20260410T120000-pid9-sigsegv.txt", "kept: has companion");

    auto cfg = baseConfig(dir, "");
    cfg.maxRetainedDumps = 3;
    CrashReporter::init(cfg);
    CrashReporter::shutdown();

    int orphansRemaining = 0;
    int txtRemaining = 0;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.find("-terminate.json") != std::string::npos) ++orphansRemaining;
        if (fn.find("-terminate.txt") != std::string::npos) ++txtRemaining;
    }
    CHECK(orphansRemaining <= 3);
    CHECK(txtRemaining <= 3);
    CHECK(fileExists(dir / "20260410T120000-pid9-sigsegv.dmp"));
    CHECK(fileExists(dir / "20260410T120000-pid9-sigsegv.json"));
    CHECK(fileExists(dir / "20260410T120000-pid9-sigsegv.txt"));
    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 8: no DSN leaves dumps untouched ─────────────────────────
static void testNoDsn() {
    std::cout << "  test: no-DSN init leaves dumps untouched ... ";
    auto dir = makeTempDir("no_dsn");
    createFile(dir / "20260601T120000-pid7-sigsegv.dmp", "MDMP gate test");
    CHECK(!CrashReporter::isSentryActive());
    CrashReporter::init(baseConfig(dir, ""));
    CHECK(CrashReporter::isInitialized());
    CHECK(!CrashReporter::isSentryActive());
    CrashReporter::shutdown();
    CHECK(fileExists(dir / "20260601T120000-pid7-sigsegv.dmp"));
    CHECK(!fileExists(dir / "20260601T120000-pid7-sigsegv.dmp.sent"));
    cleanDir(dir);
    std::cout << "done\n";
}

// ── Test 9: pending .dmp retention in local-only mode ─────────────
static void testPendingDumpRetention() {
    std::cout << "  test: pending .dmp bounded retention ... ";
    auto dir = makeTempDir("pending_retention");
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 5; ++i) {
        const std::string base = "2026050" + std::to_string(i + 1) + "T120000-pid" + std::to_string(i) + "-sigsegv";
        createFile(dir / (base + ".dmp"), "MDMP pending " + std::to_string(i));
        createFile(dir / (base + ".json"), R"({"i":)" + std::to_string(i) + "}");
        fs::last_write_time(dir / (base + ".dmp"), now - std::chrono::hours(24) * (10 - i), ec);
    }
    auto cfg = baseConfig(dir, "");
    cfg.maxRetainedDumps = 3;
    CrashReporter::init(cfg);
    CrashReporter::shutdown();

    int remaining = 0;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".dmp") == 0) ++remaining;
    }
    CHECK(remaining == 3);
    CHECK(fileExists(dir / "20260505T120000-pid4-sigsegv.dmp"));
    CHECK(fileExists(dir / "20260505T120000-pid4-sigsegv.json"));
    CHECK(!fileExists(dir / "20260501T120000-pid0-sigsegv.dmp"));
    CHECK(!fileExists(dir / "20260501T120000-pid0-sigsegv.json"));
    cleanDir(dir);
    std::cout << "done\n";
}

int main() {
    std::cout << "crash_reporter_pending_upload_test\n";

    testDsnAndMultipart();
    testLegacyRecovery();
    if (crash_upload::httpClientAvailable()) {
        testDelivered();
        testRejectedAndServerError();
        testUnreachable();
        testPerLaunchCap();
    } else {
        std::cout << "  (no HTTP client in this build; upload tests skipped)\n";
    }
    testBoundedRetention();
    testOrphanSidecarRetention();
    testNoDsn();
    testPendingDumpRetention();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}

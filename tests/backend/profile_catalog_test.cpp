#include "backend/app/BackendFacade.h"
#include "support/assert.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#endif
int main() {
    mib::test::Watchdog watchdog;
    auto fetch = [](const std::string& url) {
        return nlohmann::json::parse(backend::bridge::BackendFacade::fetchProfileCatalogUrl(url));
    };
    MIB_EXPECT(!fetch("file:///tmp/profile.json")["ok"], "only HTTP transports accepted");
    MIB_EXPECT(!fetch("http://user:password@127.0.0.1/profile.json")["ok"],
               "credential URLs refused");
#ifndef _WIN32
    if(fetch("http://127.0.0.1:1/").value("error",std::string()).find("no HTTP")!=std::string::npos)return mib::test::exitCode();
    auto fixture = [&](int status, const std::string& body) {
        int listener = socket(AF_INET, SOCK_STREAM, 0);
        MIB_REQUIRE(listener >= 0, "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        MIB_REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                    "bind");
        MIB_REQUIRE(listen(listener, 1) == 0, "listen");
        socklen_t length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        const std::string response = "HTTP/1.1 " + std::to_string(status) +
                                     " Status\r\nContent-Length: " + std::to_string(body.size()) +
                                     "\r\nConnection: close\r\n\r\n" + body;
        std::thread server([&] {
            int client = accept(listener, nullptr, nullptr);
            if (client >= 0) {
                char request[4096];
                recv(client, request, sizeof(request), 0);
                size_t offset = 0;
                while (offset < response.size()) {
                    const auto sent = send(client, response.data() + offset,
                                           response.size() - offset, MSG_NOSIGNAL);
                    if (sent <= 0) break;
                    offset += sent;
                }
                close(client);
            }
            close(listener);
        });
        const auto result =
            fetch("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/catalog.json");
        server.join();
        return result;
    };
    const auto success = fixture(200, "{\"catalog_schema_version\":1}");
    MIB_EXPECT(success["ok"] && success["body"] == "{\"catalog_schema_version\":1}",
               "successful HTTP body exact");
    MIB_EXPECT(!fixture(404, "missing")["ok"], "HTTP failure not accepted as catalog");
    MIB_EXPECT(!fixture(200, std::string(4 * 1024 * 1024 + 1, 'x'))["ok"], "response size bounded");
#endif
    return mib::test::exitCode();
}

// Fake SIGLENT SSG3021X for RfGeneratorService tests: a scripted instrument
// answering the documented SCPI subset (programming guide 3.4.8), an
// in-memory IScpiTransport straight onto it, and a loopback TCP server that
// speaks the LAN socket protocol so the real "lan" transport can be driven.
// Header-only; include as "support/fake_ssg.h".
#pragma once

#include "backend/services/ScpiTransport.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace mib::test {

using backend::services::scpi::IScpiTransport;

// Scripted SSG3021X: holds the documented state and answers the query forms
// the service uses. Faults are switches the tests flip.
struct FakeSsg {
    std::mutex m;
    std::string idn{"Siglent Technologies,SSG3021X,SSG3XBAX1R0001,3.1.21"};
    bool rfOn{true};
    bool pulm{true};
    std::string source{"INTernal"};
    std::string mode{"SINGle"};
    std::string trigMode{"EXTernal"};
    std::string slope{"POSitive"};
    double delayS{140e-9};
    double widthS{50e-6};
    double periodS{10e-3};
    bool pulseOut{true};
    double freqHz{1.2e9};
    double powDbm{-3.0};
    // faults
    bool silent{false};        // never answer queries
    bool garbage{false};       // answer with junk
    double clampWidthTo{0.0};  // >0: any width write lands here (instrument clamp)
    std::vector<std::string> log; // commands received in order

    static std::string up(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string fmt(double v) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.10g", v);
        return b;
    }
    static double parseSeconds(const std::string& arg) {
        char* end = nullptr;
        const double v = std::strtod(arg.c_str(), &end);
        std::string unit(end);
        while (!unit.empty() && unit.front() == ' ') unit.erase(0, 1);
        if (unit == "ns") return v * 1e-9;
        if (unit == "us") return v * 1e-6;
        if (unit == "ms") return v * 1e-3;
        return v;
    }
    // Returns true and sets reply when the command was a query.
    bool handle(const std::string& line, std::string& reply) {
        std::lock_guard<std::mutex> lk(m);
        if (line.empty()) return false;
        log.push_back(line);
        const std::string u = up(line);
        auto arg = [&]() {
            const auto sp = line.find(' ');
            return sp == std::string::npos ? std::string() : line.substr(sp + 1);
        };
        if (u.back() == '?') {
            if (silent) return false;
            if (garbage) {
                reply = "?!#";
                return true;
            }
            if (u == "*IDN?") reply = idn;
            else if (u == "*OPC?") reply = "1";
            else if (u == ":OUTPUT?") reply = rfOn ? "1" : "0";
            else if (u == ":PULM:STATE?") reply = pulm ? "1" : "0";
            else if (u == ":PULM:SOURCE?") reply = source;
            else if (u == ":PULM:MODE?") reply = mode;
            else if (u == ":PULM:TRIGGER:MODE?") reply = trigMode;
            else if (u == ":PULM:TRIGGER:EXTERNAL:SLOPE?") reply = slope;
            else if (u == ":PULM:DELAY?") reply = fmt(delayS);
            else if (u == ":PULM:WIDTH?") reply = fmt(widthS);
            else if (u == ":PULM:PERIOD?") reply = fmt(periodS);
            else if (u == ":PULM:OUT:STATE?") reply = pulseOut ? "1" : "0";
            else if (u == ":FREQUENCY?") reply = fmt(freqHz);
            else if (u == ":POWER?") reply = fmt(powDbm);
            else reply = "";
            return true;
        }
        if (u.rfind(":PULM:DELAY ", 0) == 0) delayS = parseSeconds(arg());
        else if (u.rfind(":PULM:WIDTH ", 0) == 0) widthS = clampWidthTo > 0 ? clampWidthTo : parseSeconds(arg());
        return false;
    }
    bool sawPulmCommand() {
        std::lock_guard<std::mutex> lk(m);
        for (const auto& l : log) {
            if (up(l).rfind(":PULM", 0) == 0) return true;
        }
        return false;
    }
};

// In-memory transport straight onto the fake instrument.
class FakeSsgTransport final : public IScpiTransport {
public:
    explicit FakeSsgTransport(FakeSsg& ssg, std::atomic<bool>* dropLink) : ssg_(ssg), dropLink_(dropLink) {}
    bool open(const std::string& resource, std::string* error) override {
        if (resource == "missing") {
            if (error) *error = "open missing failed: no such device";
            return false;
        }
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    bool write(std::string_view command, std::string* error) override {
        if (!open_) {
            if (error) *error = "not open";
            return false;
        }
        if (dropLink_ && dropLink_->load()) {
            if (error) *error = "send failed: broken pipe";
            open_ = false;
            return false;
        }
        std::string reply;
        pendingHasReply_ = ssg_.handle(std::string(command), reply);
        pending_ = reply;
        return true;
    }
    bool readLine(std::string& reply, std::chrono::milliseconds, std::string* error) override {
        if (!open_) {
            if (error) *error = "not open";
            return false;
        }
        if (!pendingHasReply_) {
            if (error) *error = "timeout waiting for reply";
            return false;
        }
        reply = pending_;
        pendingHasReply_ = false;
        return true;
    }
    std::string describe() const override { return "fake"; }

private:
    FakeSsg& ssg_;
    std::atomic<bool>* dropLink_;
    bool open_{false};
    bool pendingHasReply_{false};
    std::string pending_;
};

#ifndef _WIN32
// Loopback TCP server speaking the instrument's line protocol.
class LoopbackSsgServer {
public:
    explicit LoopbackSsgServer(FakeSsg& ssg) : ssg_(ssg) {}
    ~LoopbackSsgServer() { stop(); }
    bool start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        if (::listen(listenFd_, 1) < 0) return false;
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        running_.store(false);
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        dropClient();
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }
    // Unblock the server thread's recv() on the current client; that thread
    // owns the descriptor and closes it under m_. Closing it here raced with
    // the blocked recv() (TSan) and could hand a reused fd to recv().
    void dropClient() {
        std::lock_guard<std::mutex> lk(m_);
        if (client_ >= 0) ::shutdown(client_, SHUT_RDWR);
    }
    std::atomic<int> connections{0};

private:
    void loop() {
        while (running_.load()) {
            const int c = ::accept(listenFd_, nullptr, nullptr);
            if (c < 0) break;
            {
                std::lock_guard<std::mutex> lk(m_);
                client_ = c;
            }
            connections.fetch_add(1);
            std::string buf;
            char chunk[256];
            for (;;) {
                const ssize_t n = ::recv(c, chunk, sizeof(chunk), 0);
                if (n <= 0) break;
                buf.append(chunk, static_cast<size_t>(n));
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    std::string reply;
                    if (ssg_.handle(line, reply)) {
                        reply.push_back('\n');
                        // Two writes on purpose: exercises the reader's
                        // partial-line assembly.
                        const size_t half = reply.size() / 2;
                        if (::send(c, reply.data(), half, MSG_NOSIGNAL) < 0) break;
                        if (::send(c, reply.data() + half, reply.size() - half, MSG_NOSIGNAL) < 0) break;
                    }
                }
            }
            std::lock_guard<std::mutex> lk(m_);
            if (client_ == c) {
                ::close(c);
                client_ = -1;
            }
        }
    }
    FakeSsg& ssg_;
    int listenFd_{-1};
    int client_{-1};
    int port_{0};
    std::mutex m_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
#endif

} // namespace mib::test

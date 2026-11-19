// Local control for the desktop app: CLI tools and AI agents (MCP) on the same PC.
//
// A TCP server on 127.0.0.1 (default port 47800, never other hosts) with its own
// RemoteControl: each line a client sends is one SysEx message of the remote protocol
// (docs/SYSEX.md) as hex bytes ("F0 7D 48 53 ... F7"); replies and change notifications
// come back the same way to every connected client. desktop/tools/hsynth_remote.py
// (--tcp) and desktop/tools/hsynth_mcp.py use it; no MIDI loopback driver needed.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsynth/remote.h"

class LocalControl {
public:
    static constexpr int kDefaultPort = 47800;
    LocalControl(hsynth::Engine& engine, hsynth::RemoteControl::Config config, hsynth::RemoteControl::Hooks hooks);
    ~LocalControl();
    LocalControl(const LocalControl&) = delete;
    LocalControl& operator=(const LocalControl&) = delete;

    // Listens on 127.0.0.1:port. false + error if the port is taken.
    bool start(int port, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return port_; }
    int clients() const { return clients_.load(); }
    uint32_t requests() const { return requests_.load(); }
    hsynth::RemoteControl& control() { return *control_; }

private:
    void acceptLoop();
    void clientLoop(intptr_t sock);
    void broadcast(const std::vector<uint8_t>& msg);

    std::unique_ptr<hsynth::RemoteControl> control_;
    std::atomic<bool> running_{false};
    intptr_t listen_ = -1;
    int port_ = 0;
    std::thread acceptThread_;
    std::mutex mutex_;  // clients' sockets and threads
    std::vector<intptr_t> socks_;
    std::vector<std::thread> threads_;
    std::atomic<int> clients_{0};
    std::atomic<uint32_t> requests_{0};
};

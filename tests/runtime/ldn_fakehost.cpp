// Diagnostic: a pure ldn access point over LAN that never scans, shaped like
// the network an LGP console advertises (docs/analysis/44-multiplayer-stand.md,
// section "Abgleich mit pokeldn", the comparison with pokeldn). Started next to
// one lgp_game instance, it answers that instance's scans; whether the game
// then calls Connect separates "our scan answer is rejected" from "two
// searching instances never pick roles".
// It drives the real ldn:u service over IPC, as a guest would. No game data:
// only the advertisement's public 24-byte header with random values.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ipc_client.hpp"
#include "lgp/hle/services.hpp"
#include "lgp/platform/lan.hpp"
#include "lgp/runtime/log.hpp"

using namespace lgp_test;
namespace platform = lgp::platform;

namespace {

constexpr size_t kInfoSize = 0x480;
constexpr int64_t kLgpLcid = 0x0100'03F0'03A3'4000;

struct Options {
    uint16_t port = 39872;
    std::vector<platform::LanEndpoint> peers;
    uint16_t scene = 2;  // dreimal Pikachu, single battle
    int16_t channel = -1;
    int16_t version = 0;
    int seconds = 300;
};

int usage() {
    std::fprintf(stderr,
                 "usage: lgp_ldn_fakehost [--port P] [--peer IP:PORT]... [--scene N] [--channel C]\n"
                 "                        [--version V] [--seconds S]\n"
                 "  hosts an LGP-shaped ldn network that never scans (channel default {1,6,11}[scene %% 3])\n");
    return 2;
}

Reply call(Env& e, Handle h, uint32_t command, Msg m = {}) {
    m.command = command;
    const std::optional<Reply> r = e.send(h, m);
    return r ? *r : Reply{};
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (v == nullptr)
            return usage();
        ++i;
        if (a == "--port") {
            o.port = static_cast<uint16_t>(std::atoi(v));
        } else if (a == "--peer") {
            platform::LanEndpoint ep;
            if (!platform::parse_lan_endpoint(v, &ep))
                return usage();
            o.peers.push_back(ep);
        } else if (a == "--scene") {
            o.scene = static_cast<uint16_t>(std::atoi(v));
        } else if (a == "--channel") {
            o.channel = static_cast<int16_t>(std::atoi(v));
        } else if (a == "--version") {
            o.version = static_cast<int16_t>(std::atoi(v));
        } else if (a == "--seconds") {
            o.seconds = std::atoi(v);
        } else {
            return usage();
        }
    }
    if (o.channel < 0) {
        static const int16_t kChannels[] = {1, 6, 11};
        o.channel = kChannels[o.scene % 3];
    }
    lgp::rt::set_log_level(lgp::rt::LogLevel::Debug);

    LdnLanConfig cfg;
    cfg.lan = platform::make_host_lan();
    if (!cfg.lan) {
        std::fprintf(stderr, "no host sockets\n");
        return 1;
    }
    cfg.port = o.port;
    cfg.peers = o.peers;
    Env e([&cfg](ServiceManager& sm) {
        SystemConfig config;
        register_base_services(sm, config, small_process());
        register_local_network(sm, cfg);
    });
    const Handle ldn = [&] {
        Msg m;
        m.command = 0;
        return e.send(e.open_service("ldn:u"), m)->move.at(0);
    }();

    Msg init;
    init.send_pid = true;
    init.arg<uint64_t>(0);
    if (call(e, ldn, 400, init).result != 0 || call(e, ldn, 200).result != 0) {
        std::fprintf(stderr, "Initialize/OpenAccessPoint failed\n");
        return 1;
    }

    // Advertisement header (pokeldn docs/lgpe_session.md, "The session key"):
    // network id (random per session), CRC32 of the user password (0),
    // system communication version 4, header size 0x18, then the session
    // param at +0xC; 8 bytes of zero application data behind it.
    std::random_device rd;
    std::vector<uint8_t> adv(0x20, 0);
    const uint32_t network_id = rd(), session_param = rd();
    std::memcpy(adv.data() + 0x0, &network_id, 4);
    adv[0x8] = 4;
    adv[0x9] = 0x18;
    std::memcpy(adv.data() + 0xC, &session_param, 4);
    CHECK(e.k.memory().copy_to_user(kBufA, adv.data(), adv.size()));
    Msg advertise;
    advertise.a = {{kBufA, adv.size(), 0}};
    if (call(e, ldn, 206, advertise).result != 0) {
        std::fprintf(stderr, "SetAdvertiseData failed\n");
        return 1;
    }

    // CreateNetworkPrivate(SecurityConfig, SecurityParameter, UserConfig, pad,
    // NetworkConfig): session 01 00.. as LGP passes it, two nodes.
    std::vector<uint8_t> p(0xB8, 0);
    const uint16_t security_mode = 1;
    std::memcpy(p.data(), &security_mode, 2);
    p[0x44 + 0x10] = 0x01;                              // SecurityParameter.session_id
    std::memcpy(p.data() + 0x64, "fakehost", 8);        // UserConfig
    std::memcpy(p.data() + 0x98, &kLgpLcid, 8);         // NetworkConfig.intent_id
    std::memcpy(p.data() + 0x98 + 0xA, &o.scene, 2);
    std::memcpy(p.data() + 0x98 + 0x10, &o.channel, 2);
    p[0x98 + 0x12] = 2;
    std::memcpy(p.data() + 0x98 + 0x14, &o.version, 2);
    Msg create;
    create.payload = p;
    if (call(e, ldn, 203, create).result != 0) {
        std::fprintf(stderr, "CreateNetworkPrivate failed\n");
        return 1;
    }
    std::printf("fakehost: hosting lcid 0x%llx scene %u channel %d version %d on UDP port %u for %d s\n",
                static_cast<unsigned long long>(kLgpLcid), o.scene, o.channel, o.version, o.port, o.seconds);
    std::fflush(stdout);

    uint8_t last_nodes = 1;
    for (int t = 0; t < o.seconds * 10; ++t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Msg get;
        get.c = {{kBufB, kInfoSize}};
        if (call(e, ldn, 1, get).result != 0)
            continue;
        uint8_t nodes = 0;
        CHECK(e.k.memory().copy_from_user(&nodes, kBufB + 0x67, 1));
        if (nodes != last_nodes) {
            std::printf("fakehost: %.1f s: %u node(s) in the network%s\n", t / 10.0, nodes,
                        nodes > last_nodes ? " -- the game joined" : "");
            std::fflush(stdout);
            last_nodes = nodes;
        }
    }
    call(e, ldn, 204);  // DestroyNetwork
    std::printf("fakehost: done\n");
    return 0;
}

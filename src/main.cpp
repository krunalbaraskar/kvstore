#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "server.h"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --port N                  TCP port (default 6380)\n"
                 "  --bind ADDR               IPv4 address to listen on (default 127.0.0.1)\n"
                 "  --appendonly yes|no       enable the append-only file (default yes)\n"
                 "  --aof-file PATH           AOF path (default appendonly.aof)\n"
                 "  --fsync always|everysec|no  AOF fsync policy (default everysec)\n"
                 "  --pubsub-output-limit N   bytes of unsent output before a subscriber\n"
                 "                            is disconnected (default 8388608)\n",
                 prog);
}

template <class T>
bool parse_number(std::string_view s, T& out) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string_view opt = argv[i];
        if (opt == "-h" || opt == "--help") return false;
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing value for %s\n", argv[i]);
            return false;
        }
        std::string_view val = argv[++i];
        bool ok = true;
        if (opt == "--port") {
            ok = parse_number(val, cfg.port);
        } else if (opt == "--bind") {
            cfg.bind = val;
        } else if (opt == "--appendonly") {
            ok = val == "yes" || val == "no";
            cfg.appendonly = val == "yes";
        } else if (opt == "--aof-file") {
            cfg.aof_path = val;
        } else if (opt == "--fsync") {
            if (val == "always") {
                cfg.fsync = FsyncPolicy::Always;
            } else if (val == "everysec") {
                cfg.fsync = FsyncPolicy::EverySec;
            } else if (val == "no") {
                cfg.fsync = FsyncPolicy::No;
            } else {
                ok = false;
            }
        } else if (opt == "--pubsub-output-limit") {
            ok = parse_number(val, cfg.pubsub_output_limit);
        } else {
            std::fprintf(stderr, "Unknown option %s\n", argv[i - 1]);
            return false;
        }
        if (!ok) {
            std::fprintf(stderr, "Invalid value for %s: %s\n", argv[i - 1], argv[i]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        usage(argv[0]);
        return 2;
    }
    Server server(std::move(cfg));
    return server.run();
}

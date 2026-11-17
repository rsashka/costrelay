#include "app/App.h"
#include "auth/Auth.h"
#include "config/Config.h"
#include "config/DefaultConfig.h"
#include "util/Strings.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>
#ifndef _WIN32
#include <termios.h>
#endif

namespace {

void usage(std::ostream& os = std::cerr) {
    os
        << "Usage: costrelayd [options]\n"
        << "\n"
        << "  --config FILE         run with FILE (default: config/costrelay.yaml)\n"
        << "  --check               validate FILE and exit\n"
        << "  --init-config [FILE]  write commented default config (default: config/costrelay.yaml)\n"
        << "  --force               overwrite FILE for --init-config\n"
        << "  --hash-password       read password, print Argon2id hash\n"
        << "  --hash-token [ID]     generate Bearer token and sha256 hash\n"
        << "  --id NAME             token id for --hash-token\n"
        << "  --help, -h            this help\n"
        << "\n"
        << "Config options are documented in the file produced by --init-config.\n"
        << "Secrets in config: file:/path or env:NAME only.\n"
        << "--hash-password prompts twice without echo on a terminal, or reads one\n"
        << "line from stdin in a pipeline.\n";
}

bool writeDefaultConfig(const std::string& path, bool force) {
    const std::filesystem::path target(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    if (std::filesystem::exists(target, ec) && !force) {
        std::cerr << "config exists: " << path << " (use --force to overwrite)\n";
        return false;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cannot write config: " << path << "\n";
        return false;
    }
    out << proxy::kDefaultConfig;
    if (!out.good()) {
        std::cerr << "cannot write config: " << path << "\n";
        return false;
    }
    std::cout << "wrote " << path << "\n";
    return true;
}

bool readPasswordLine(const std::string& prompt, std::string& out) {
    if (!prompt.empty()) {
        std::cerr << prompt << std::flush;
    }
    bool tty = false;
#ifndef _WIN32
    tty = isatty(STDIN_FILENO) != 0;
    struct termios old {};
    if (tty && tcgetattr(STDIN_FILENO, &old) == 0) {
        struct termios quiet = old;
        quiet.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
#endif
    const bool ok = static_cast<bool>(std::getline(std::cin, out));
#ifndef _WIN32
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::cerr << "\n";
    }
#endif
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return ok;
}

bool readPasswordInteractive(std::string& password) {
#ifndef _WIN32
    if (!isatty(STDIN_FILENO)) return readPasswordLine({}, password);
    std::string confirm;
    if (!readPasswordLine("Password: ", password)) return false;
    if (password.empty()) {
        std::cerr << "empty password\n";
        return false;
    }
    if (!readPasswordLine("Repeat password: ", confirm)) return false;
    if (password != confirm) {
        std::cerr << "passwords do not match\n";
        return false;
    }
    return true;
#else
    return readPasswordLine("Password: ", password);
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/costrelay.yaml";
    bool check = false;
    bool hash_password = false;
    bool init_config = false;
    bool force = false;
    std::string init_path = "config/costrelay.yaml";
    std::string hash_token_id;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--check") check = true;
        else if (a == "--init-config") {
            init_config = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') init_path = argv[++i];
        } else if (a == "--force") force = true;
        else if (a == "--hash-password") hash_password = true;
        else if (a == "--hash-token") {
            hash_token_id = "token";
            if (i + 1 < argc && argv[i + 1][0] != '-') hash_token_id = argv[++i];
        } else if (a == "--id" && i + 1 < argc) {
            hash_token_id = argv[++i];
        } else if (a == "--help" || a == "-h") { usage(std::cout); return 0; }
        else { std::cerr << "unknown argument: " << a << "\n"; usage(); return 2; }
    }

    try {
        if (init_config) return writeDefaultConfig(init_path, force) ? 0 : 2;
        if (hash_password) {
            std::string password;
            if (readPasswordInteractive(password)) {
                if (password.empty()) { std::cerr << "empty password\n"; return 2; }
                std::cout << proxy::PasswordHash::hash(password) << "\n";
                return 0;
            }
            if (!password.empty()) return 2;
            std::cerr << "empty password\n";
            return 2;
        }
        if (!hash_token_id.empty()) {
            const std::string token = "pxy_" + hash_token_id + "_" + proxy::randomBase64Url(32);
            std::cout << "token: " << token << "\n";
            std::cout << "token_hash: sha256:" << proxy::sha256Hex(token) << "\n";
            return 0;
        }
        if (!std::filesystem::exists(config_path)) {
            std::cerr << "config not found: " << config_path
                      << " (run --init-config)\n";
            return 2;
        }
        auto config = proxy::AppConfig::load(config_path);
        if (check) {
            std::cout << "config OK: " << config.routers.size() << " router(s), "
                      << config.routing.models.size() << " model(s)\n";
            return 0;
        }
        proxy::ProxyApp app(std::move(config));
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}

#include "app/ServiceControl.h"

#include <filesystem>

namespace proxy {
namespace {

constexpr const char* kProxyServiceKey = "proxy";

} // namespace

const std::vector<ServiceTarget>& serviceTargets() {
    static const std::vector<ServiceTarget> targets = {
        {"dsh", "DSH", "dsh-web.service"},
        {"strata", "Strata", "strata.service"},
        {kProxyServiceKey, "CostRelay", "costrelayd.service"},
    };
    return targets;
}

bool deferredRestart(const std::string& key) { return key == kProxyServiceKey; }

const ServiceTarget* findServiceTarget(const std::string& key) {
    for (const auto& target : serviceTargets()) {
        if (key == target.key) return &target;
    }
    return nullptr;
}

bool systemUnitInstalled(const std::string& unit) {
    const char* directories[] = {"/etc/systemd/system/", "/lib/systemd/system/",
                                 "/usr/lib/systemd/system/"};
    for (const auto* directory : directories) {
        if (std::filesystem::exists(std::string(directory) + unit)) return true;
    }
    return false;
}

std::vector<std::string> restartServiceCommand(const std::string& unit) {
    return {"/usr/bin/sudo",     "-n", "/usr/bin/systemctl",
            "restart",          "--no-block", unit};
}

} // namespace proxy
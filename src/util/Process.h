#pragma once

#include <string>
#include <vector>

namespace proxy {

struct ProcessResult {
    int exit_code = -1;
    std::string output;
    // Вывод обрезан по пределу буфера: факт виден вызывающему, а не молчит.
    bool truncated = false;

    bool ok() const { return exit_code == 0; }
};

// Runs an executable without a shell and captures stdout/stderr. The caller is
// responsible for passing absolute paths and a fixed argument list.
ProcessResult runProcess(const std::vector<std::string>& args);

} // namespace proxy
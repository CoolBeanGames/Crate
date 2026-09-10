#include "core/Log.h"
#include <cstdio>

namespace crate::log {

static void stdoutSink(const Console::Entry& e) {
    const char* lvl = e.level == Console::Level::Error ? "ERR"
                      : e.level == Console::Level::Warn ? "WRN"
                                                       : "inf";
    const char* ch = e.channel == Console::Channel::Game ? "game" : "engine";
    std::printf("[%8.3f] %-6s %-3s %-8s  %s\n", e.time, ch, lvl, e.category.c_str(),
                e.text.c_str());
    std::fflush(stdout);
}

void enableStdoutMirror() { Console::get().setSink(&stdoutSink); }

} // namespace crate::log

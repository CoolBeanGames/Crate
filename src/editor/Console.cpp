#include "editor/Console.h"
#include <algorithm>
#include <chrono>

namespace crate {

static std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

Console& Console::get() {
    static Console instance;
    return instance;
}

double Console::now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

void Console::log(Channel ch, Level lvl, std::string category, std::string text) {
    Entry e{ch, lvl, now(), std::move(category), std::move(text)};
    if (sink_)
        sink_(e);
    entries_.push_back(std::move(e));
    // Keep the buffer bounded so a long session does not grow without limit.
    if (entries_.size() > 5000)
        entries_.erase(entries_.begin(), entries_.begin() + 1000);
}

void Console::clear(Channel ch) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [ch](const Entry& e) { return e.channel == ch; }),
                   entries_.end());
}

} // namespace crate

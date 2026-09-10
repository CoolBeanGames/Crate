#include "editor/Console.h"
#include <algorithm>

namespace crate {

Console& Console::get() {
    static Console instance;
    return instance;
}

void Console::log(Channel ch, Level lvl, std::string text) {
    entries_.push_back({ch, lvl, std::move(text)});
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

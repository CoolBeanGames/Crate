#pragma once
#include <string>
#include <vector>

namespace crate {

// Shared log sink for the Engine Console and Game Console panels. Two channels
// keep editor/engine chatter separate from in-game script output.
class Console {
public:
    enum class Channel { Engine, Game };
    enum class Level { Info, Warn, Error };

    struct Entry {
        Channel channel;
        Level level;
        std::string text;
    };

    static Console& get();

    void log(Channel ch, Level lvl, std::string text);
    void info(Channel ch, std::string t) { log(ch, Level::Info, std::move(t)); }
    void warn(Channel ch, std::string t) { log(ch, Level::Warn, std::move(t)); }
    void error(Channel ch, std::string t) { log(ch, Level::Error, std::move(t)); }

    const std::vector<Entry>& entries() const { return entries_; }
    void clear(Channel ch);

private:
    Console() = default;
    std::vector<Entry> entries_;
};

} // namespace crate

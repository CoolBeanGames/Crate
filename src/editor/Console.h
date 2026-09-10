#pragma once
#include <string>
#include <vector>

namespace crate {

// The engine-wide system log. Everything the engine does of note is routed here
// and surfaced in the Engine Console / Game Console panels. Two channels keep
// editor/engine chatter separate from in-game script output.
//
// Prefer the CR_LOG* macros (Log.h) over calling this directly.
class Console {
public:
    enum class Channel { Engine, Game };
    enum class Level { Info, Warn, Error };

    struct Entry {
        Channel channel;
        Level level;
        double time;          // seconds since engine start
        std::string category; // short tag, e.g. "scene", "assets", "render"
        std::string text;
    };

    static Console& get();

    // Seconds since the process started, as stamped on entries.
    static double now();

    void log(Channel ch, Level lvl, std::string category, std::string text);

    // Convenience: Engine channel, "app" category.
    void info(Channel ch, std::string t) { log(ch, Level::Info, "app", std::move(t)); }
    void warn(Channel ch, std::string t) { log(ch, Level::Warn, "app", std::move(t)); }
    void error(Channel ch, std::string t) { log(ch, Level::Error, "app", std::move(t)); }

    const std::vector<Entry>& entries() const { return entries_; }
    void clear(Channel ch);

    // Callback invoked for every entry (used to mirror the log to stdout / a
    // file). Set to nullptr to disable.
    void setSink(void (*sink)(const Entry&)) { sink_ = sink; }

private:
    Console() = default;
    std::vector<Entry> entries_;
    void (*sink_)(const Entry&) = nullptr;
};

} // namespace crate

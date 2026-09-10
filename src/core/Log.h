#pragma once
#include "editor/Console.h"
#include <string>

// Engine-wide logging front end. These macros are the intended way to record
// what the engine is doing; they forward to the Console system log.
//
//   CR_LOG("scene", "Loaded " + name);        // info, Engine channel
//   CR_WARN("assets", "Unknown extension");
//   CR_ERROR("render", "Device lost");
//   CR_GAME("script", "player died");          // Game channel (script output)

namespace crate::log {

inline void write(Console::Channel ch, Console::Level lvl, std::string cat, std::string msg) {
    Console::get().log(ch, lvl, std::move(cat), std::move(msg));
}

// Install a sink that also prints the log to the process stdout.
void enableStdoutMirror();

} // namespace crate::log

#define CR_LOG(cat, msg) \
    ::crate::log::write(::crate::Console::Channel::Engine, ::crate::Console::Level::Info, (cat), (msg))
#define CR_WARN(cat, msg) \
    ::crate::log::write(::crate::Console::Channel::Engine, ::crate::Console::Level::Warn, (cat), (msg))
#define CR_ERROR(cat, msg) \
    ::crate::log::write(::crate::Console::Channel::Engine, ::crate::Console::Level::Error, (cat), (msg))
#define CR_GAME(cat, msg) \
    ::crate::log::write(::crate::Console::Channel::Game, ::crate::Console::Level::Info, (cat), (msg))

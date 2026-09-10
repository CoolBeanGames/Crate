#pragma once
#include <string>

namespace crate::script {

// Re-indent cScript source: one tab per open-brace depth, lines that begin with
// a closing brace dedent first. Used when a script loads and by the editor's
// "Format" action.
std::string reindent(const std::string& src);

// Given the text of the line the caret is on (before a newline is inserted),
// return the leading whitespace the next line should start with. A trailing
// unclosed '{' adds one tab.
std::string indentForNewLine(const std::string& currentLine);

} // namespace crate::script

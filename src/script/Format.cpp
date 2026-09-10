#include "script/Format.h"

#include <sstream>

namespace crate::script {

// Count net brace delta on a line, ignoring braces inside // comments, strings
// and char literals.
static void scanLine(const std::string& line, int& openBefore, int& delta, bool& startsWithClose) {
    delta = 0;
    startsWithClose = false;
    bool seenNonSpace = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
            break;
        if (c == '"' || c == '\'') {
            char q = c;
            ++i;
            while (i < line.size() && line[i] != q) {
                if (line[i] == '\\')
                    ++i;
                ++i;
            }
            seenNonSpace = true;
            continue;
        }
        if (c == '{') {
            ++delta;
            seenNonSpace = true;
        } else if (c == '}') {
            --delta;
            if (!seenNonSpace)
                startsWithClose = true;
            seenNonSpace = true;
        } else if (c != ' ' && c != '\t') {
            seenNonSpace = true;
        }
    }
    (void)openBefore;
}

std::string reindent(const std::string& src) {
    std::istringstream in(src);
    std::string line;
    std::string out;
    int depth = 0;
    bool first = true;
    while (std::getline(in, line)) {
        // strip existing leading whitespace and a trailing '\r'
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        std::string trimmed = (s == std::string::npos) ? std::string() : line.substr(s);

        int delta = 0;
        bool startsClose = false;
        scanLine(trimmed, depth, delta, startsClose);

        int thisIndent = depth;
        if (startsClose)
            thisIndent = depth - 1;
        if (thisIndent < 0)
            thisIndent = 0;

        if (!first)
            out += '\n';
        first = false;
        if (!trimmed.empty())
            out.append(static_cast<size_t>(thisIndent), '\t');
        out += trimmed;

        depth += delta;
        if (depth < 0)
            depth = 0;
    }
    return out;
}

std::string indentForNewLine(const std::string& currentLine) {
    size_t s = currentLine.find_first_not_of(" \t");
    std::string lead = (s == std::string::npos) ? currentLine : currentLine.substr(0, s);
    // If the line's last non-space char is '{', add a level.
    for (size_t i = currentLine.size(); i-- > 0;) {
        char c = currentLine[i];
        if (c == ' ' || c == '\t' || c == '\r')
            continue;
        if (c == '{')
            lead += '\t';
        break;
    }
    return lead;
}

} // namespace crate::script

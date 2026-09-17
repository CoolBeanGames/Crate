#pragma once
// Tiny text codec for one scene-file FIELD line's value, shared by every
// Component/Actor writeFields()/readField() override (see SceneIO.cpp for
// the overall file format). Deliberately dumb: no JSON, no escaping beyond
// "value is the rest of the line" -- matches the project's existing
// hand-rolled text formats (AssetDatabase's tab-separated .assetdb).
#include <ostream>
#include <sstream>
#include <string>

namespace crate {

inline void writeField(std::ostream& out, const char* key, const char* kind, const std::string& value) {
    out << "FIELD " << key << ' ' << kind << ' ' << value << '\n';
}
inline void writeFieldInt(std::ostream& out, const char* key, long long v) {
    writeField(out, key, "i", std::to_string(v));
}
inline void writeFieldFloat(std::ostream& out, const char* key, double v) {
    std::ostringstream ss;
    ss.precision(9);
    ss << v;
    writeField(out, key, "f", ss.str());
}
inline void writeFieldBool(std::ostream& out, const char* key, bool v) {
    writeField(out, key, "b", v ? "1" : "0");
}
inline void writeFieldString(std::ostream& out, const char* key, const std::string& v) {
    writeField(out, key, "s", v);
}
inline void writeFieldVec3(std::ostream& out, const char* key, float x, float y, float z) {
    std::ostringstream ss;
    ss.precision(9);
    ss << x << ',' << y << ',' << z;
    writeField(out, key, "v3", ss.str());
}
inline void writeFieldVec2(std::ostream& out, const char* key, float x, float y) {
    std::ostringstream ss;
    ss.precision(9);
    ss << x << ',' << y;
    writeField(out, key, "v2", ss.str());
}

inline double fieldF(const std::string& s) {
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}
inline long long fieldI(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}
inline bool fieldB(const std::string& s) { return !s.empty() && s[0] == '1'; }

// Parses "a,b,c" (any of the 3 components may be omitted) into out[0..2].
inline void parseFieldVec3(const std::string& s, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    size_t a = 0;
    for (int i = 0; i < 3 && a <= s.size(); ++i) {
        size_t c = s.find(',', a);
        std::string part = s.substr(a, c == std::string::npos ? std::string::npos : c - a);
        if (!part.empty())
            out[i] = (float)fieldF(part);
        if (c == std::string::npos)
            break;
        a = c + 1;
    }
}
inline void parseFieldVec2(const std::string& s, float out[2]) {
    float v3[3];
    parseFieldVec3(s, v3);
    out[0] = v3[0];
    out[1] = v3[1];
}

} // namespace crate

#include "core/Profiler.h"

#include <algorithm>

namespace crate {

Profiler& Profiler::get() {
    static Profiler instance;
    return instance;
}

void Profiler::beginFrame() {
    actorMsThisFrame_.clear();
    componentMsThisFrame_.clear();
}

void Profiler::addSample(const char* section, double milliseconds) {
    auto it = sections_.find(section);
    if (it == sections_.end()) {
        it = sections_.emplace(section, Section{}).first;
        sectionOrder_.push_back(section);
    }
    Section& s = it->second;
    s.history[static_cast<size_t>(s.next)] = static_cast<float>(milliseconds);
    s.next = (s.next + 1) % kHistoryLen;
    if (s.count < kHistoryLen)
        ++s.count;
    s.lastMs = milliseconds;
}

void Profiler::addComponentSample(const std::string& actorName, const std::string& componentType,
                                  double milliseconds) {
    actorMsThisFrame_[actorName] += milliseconds;
    componentMsThisFrame_[componentType] += milliseconds;
}

const Profiler::SectionView* Profiler::section(const std::string& name) const {
    static thread_local SectionView view; // returned by pointer; fine single-threaded like the rest
    auto it = sections_.find(name);
    if (it == sections_.end())
        return nullptr;
    const Section& s = it->second;
    view.lastMs = s.lastMs;
    double sum = 0.0;
    for (int i = 0; i < s.count; ++i)
        sum += s.history[static_cast<size_t>(i)];
    view.avgMs = s.count > 0 ? sum / s.count : 0.0;
    return &view;
}

std::vector<std::string> Profiler::sectionNames() const { return sectionOrder_; }

int Profiler::historyOrdered(const std::string& name, float* out) const {
    auto it = sections_.find(name);
    if (it == sections_.end())
        return 0;
    const Section& s = it->second;
    // s.next is the write cursor (where the OLDEST sample will be overwritten
    // next); once the buffer has wrapped (count == kHistoryLen), that's also
    // the index of the current oldest sample.
    int start = (s.count < kHistoryLen) ? 0 : s.next;
    for (int i = 0; i < s.count; ++i)
        out[i] = s.history[static_cast<size_t>((start + i) % kHistoryLen)];
    return s.count;
}

namespace {
std::vector<Profiler::Breakdown> sortedBreakdown(const std::unordered_map<std::string, double>& m) {
    std::vector<Profiler::Breakdown> out;
    out.reserve(m.size());
    for (const auto& [name, ms] : m)
        out.push_back({name, ms});
    std::sort(out.begin(), out.end(),
             [](const Profiler::Breakdown& a, const Profiler::Breakdown& b) { return a.ms > b.ms; });
    return out;
}
} // namespace

std::vector<Profiler::Breakdown> Profiler::byActor() const { return sortedBreakdown(actorMsThisFrame_); }
std::vector<Profiler::Breakdown> Profiler::byComponentType() const {
    return sortedBreakdown(componentMsThisFrame_);
}

} // namespace crate

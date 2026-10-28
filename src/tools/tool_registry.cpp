// src/tools/tool_registry.cpp

#include "tools/tool_registry.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace {

// Bucket counts are always powers of two, so a mask does the job of a modulo.
std::size_t slot_for(std::size_t h, std::size_t bucket_count) {
    return h & (bucket_count - 1);
}

}  // namespace

ToolRegistry::ToolRegistry() : buckets_(kInitialBuckets) {}

ToolRegistry::~ToolRegistry() = default;

// FNV-1a, 64 bit.
std::size_t ToolRegistry::hash(std::string_view key) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(h);
}

const ToolRegistry::Entry* ToolRegistry::lookup(std::size_t h, std::string_view name) const {
    for (const Entry& e : buckets_[slot_for(h, buckets_.size())]) {
        if (e.hash == h && e.name == name) return &e;
    }
    return nullptr;
}

void ToolRegistry::grow() {
    std::vector<Bucket> bigger(buckets_.size() * 2);

    // Size every new bucket before moving anything. Once the moves start
    // nothing can allocate, so a bad_alloc can't leave tools half-moved.
    std::vector<std::size_t> needed(bigger.size(), 0);
    for (const Bucket& bucket : buckets_) {
        for (const Entry& e : bucket) ++needed[slot_for(e.hash, bigger.size())];
    }
    for (std::size_t i = 0; i < bigger.size(); ++i) bigger[i].reserve(needed[i]);

    for (Bucket& bucket : buckets_) {
        for (Entry& e : bucket) {
            bigger[slot_for(e.hash, bigger.size())].push_back(std::move(e));
        }
    }
    buckets_ = std::move(bigger);
}

bool ToolRegistry::add(std::unique_ptr<Tool> tool) {
    if (!tool) return false;

    std::string name = tool->name();
    const std::size_t h = hash(name);
    if (lookup(h, name) != nullptr) return false;

    // Grow first, so the load factor never goes past 3/4, even briefly.
    if ((count_ + 1) * 4 > buckets_.size() * 3) grow();

    buckets_[slot_for(h, buckets_.size())].push_back(Entry{h, std::move(name), std::move(tool)});
    ++count_;
    return true;
}

Tool* ToolRegistry::find(std::string_view name) const {
    const Entry* e = lookup(hash(name), name);
    return e ? e->tool.get() : nullptr;
}

std::size_t ToolRegistry::size() const noexcept {
    return count_;
}

void ToolRegistry::for_each(const std::function<void(const Tool&)>& fn) const {
    for (const Bucket& bucket : buckets_) {
        for (const Entry& e : bucket) fn(*e.tool);
    }
}

std::size_t ToolRegistry::bucket_count() const noexcept {
    return buckets_.size();
}

double ToolRegistry::load_factor() const noexcept {
    return static_cast<double>(count_) / static_cast<double>(buckets_.size());
}

std::size_t ToolRegistry::max_probe_length() const {
    std::size_t longest = 0;
    for (const Bucket& bucket : buckets_) longest = std::max(longest, bucket.size());
    return longest;
}

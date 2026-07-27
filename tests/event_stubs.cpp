// Test-only definitions for lifecycle events.
//
// Production universal-module builds generate the `logos_events:` method body.
// Unit tests compile the implementation directly, so retain emitted payloads
// here to assert protocol ordering and redaction without a Logos host.

#include "lez_indexer_module_impl.h"

#include <mutex>
#include <string>
#include <vector>

namespace {
    std::mutex node_changed_events_mutex;
    std::vector<std::string> node_changed_events;
} // namespace

void reset_node_changed_events() {
    std::lock_guard<std::mutex> lock(node_changed_events_mutex);
    node_changed_events.clear();
}

std::vector<std::string> get_node_changed_events() {
    std::lock_guard<std::mutex> lock(node_changed_events_mutex);
    return node_changed_events;
}

void LezIndexerModuleImpl::nodeChanged(const std::string& event) {
    std::lock_guard<std::mutex> lock(node_changed_events_mutex);
    node_changed_events.push_back(event);
}

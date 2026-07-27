#include <logos_test.h>

#include "lez_indexer_module_impl.h"
#include <indexer_ffi.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

void reset_node_changed_events();
std::vector<std::string> get_node_changed_events();
void reset_indexer_ffi_mock();
void set_indexer_ffi_start_status(OperationStatus status);
int indexer_ffi_start_calls();
int indexer_ffi_stop_calls();

namespace {
    const std::string kChannelId(64, '1');

    struct TempDir {
        fs::path path;

        TempDir() {
            char pattern[] = "/tmp/lez-indexer-module-tests-XXXXXX";
            char* const directory = mkdtemp(pattern);
            if (directory) {
                path = directory;
            }
        }

        ~TempDir() {
            if (!path.empty()) {
                std::error_code error;
                fs::remove_all(path, error);
            }
        }

        [[nodiscard]] bool valid() const {
            return !path.empty();
        }

        [[nodiscard]] std::string filePath(const std::string& name) const {
            return (path / name).string();
        }
    };

    std::string writeIndexerConfig(const TempDir& temp_dir, const std::string& name = "indexer-config.json") {
        const std::string path = temp_dir.filePath(name);
        std::ofstream output(path);
        output << json({
                           {"bedrock_config", {{"addr", "http://127.0.0.1:8080"}}},
                           {"channel_id", kChannelId},
                           {"consensus_info_polling_interval", "1s"},
                       })
                      .dump();
        return path;
    }

    json lifecycleCommand(
        const std::string& operation_id,
        const std::string& action,
        const json& parameters = json::object()
    ) {
        json command = {
            {"schema", "logos.managed_node_lifecycle.command"},
            {"version", 1},
            {"operation_id", operation_id},
            {"action", action},
        };
        if (!parameters.empty()) {
            command["parameters"] = parameters;
        }
        return command;
    }

    json nodeStatus(LezIndexerModuleImpl& module) {
        return json::parse(module.nodeStatus());
    }

    json nodeAction(LezIndexerModuleImpl& module, const json& request) {
        return json::parse(module.nodeAction(request.dump()));
    }

    std::vector<json> lifecycleEvents() {
        std::vector<json> events;
        for (const std::string& event : get_node_changed_events()) {
            events.push_back(json::parse(event));
        }
        return events;
    }

    void setTestContext(LezIndexerModuleImpl& module, const TempDir& temp_dir) {
        module._logosCoreSetContext_("", "lifecycle-test", temp_dir.path.string());
    }
} // namespace

LOGOS_TEST(node_status_reports_uninitialized_indexer_scope) {
    reset_indexer_ffi_mock();
    LezIndexerModuleImpl module;

    const json status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("schema").get<std::string>(), std::string("logos.managed_node_lifecycle.snapshot"));
    LOGOS_ASSERT_EQ(status.at("version").get<int>(), 1);
    LOGOS_ASSERT_FALSE(status.at("instance_id").get<std::string>().empty());
    LOGOS_ASSERT_EQ(status.at("epoch").get<std::uint64_t>(), 0U);
    LOGOS_ASSERT_EQ(status.at("sequence").get<std::uint64_t>(), 0U);
    LOGOS_ASSERT_EQ(status.at("scope").at("kind").get<std::string>(), std::string("indexer"));
    LOGOS_ASSERT_TRUE(status.at("scope").at("channel_id").is_null());
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("uninitialized"));
    LOGOS_ASSERT_EQ(status.at("health").get<std::string>(), std::string("unknown"));
    LOGOS_ASSERT_EQ(status.at("supported_actions").size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(status.at("supported_actions").at(0).get<std::string>(), std::string("start"));
}

LOGOS_TEST(legacy_indexer_lifecycle_stays_compatible_with_v1_status) {
    reset_indexer_ffi_mock();
    TempDir temp_dir;
    LOGOS_ASSERT_TRUE(temp_dir.valid());
    const std::string config_path = writeIndexerConfig(temp_dir);
    LezIndexerModuleImpl module;
    setTestContext(module, temp_dir);

    LOGOS_ASSERT_EQ(module.start_indexer(config_path), 0);
    LOGOS_ASSERT_EQ(module.start_indexer(config_path), 0);
    LOGOS_ASSERT_EQ(indexer_ffi_start_calls(), 1);
    json status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_EQ(status.at("scope").at("channel_id").get<std::string>(), kChannelId);

    LOGOS_ASSERT_EQ(module.stop_indexer(), 0);
    LOGOS_ASSERT_EQ(indexer_ffi_stop_calls(), 1);
    status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("stopped"));

    LOGOS_ASSERT_EQ(module.reset_storage(config_path), 0);
    status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_EQ(status.at("scope").at("channel_id").get<std::string>(), kChannelId);
}

LOGOS_TEST(node_action_starts_stops_and_resets_a_channel_indexer) {
    reset_indexer_ffi_mock();
    TempDir temp_dir;
    LOGOS_ASSERT_TRUE(temp_dir.valid());
    const std::string config_path = writeIndexerConfig(temp_dir, "host-local-config-token.json");
    LezIndexerModuleImpl module;
    setTestContext(module, temp_dir);

    reset_node_changed_events();
    const json started =
        nodeAction(module, lifecycleCommand("indexer-start-v1", "start", {{"config_path", config_path}}));
    LOGOS_ASSERT_TRUE(started.at("accepted").get<bool>());
    LOGOS_ASSERT_FALSE(started.at("duplicate").get<bool>());
    LOGOS_ASSERT_TRUE(started.at("error").is_null());
    LOGOS_ASSERT_EQ(indexer_ffi_start_calls(), 1);

    std::vector<json> events = lifecycleEvents();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(0).at("phase").get<std::string>(), std::string("accepted"));
    LOGOS_ASSERT_EQ(events.at(0).at("status").at("state").get<std::string>(), std::string("starting"));
    LOGOS_ASSERT_EQ(events.at(1).at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("succeeded"));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_EQ(events.at(1).at("scope").at("channel_id").get<std::string>(), kChannelId);
    LOGOS_ASSERT_TRUE(
        events.at(1).at("sequence").get<std::uint64_t>() > events.at(0).at("sequence").get<std::uint64_t>()
    );
    LOGOS_ASSERT_TRUE(events.at(1).dump().find("host-local-config-token") == std::string::npos);

    json status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_EQ(status.at("health").get<std::string>(), std::string("healthy"));
    LOGOS_ASSERT_EQ(status.at("scope").at("channel_id").get<std::string>(), kChannelId);
    LOGOS_ASSERT_EQ(status.at("epoch").get<std::uint64_t>(), 1U);
    LOGOS_ASSERT_TRUE(status.dump().find("host-local-config-token") == std::string::npos);

    reset_node_changed_events();
    const json stopped = nodeAction(module, lifecycleCommand("indexer-stop-v1", "stop"));
    LOGOS_ASSERT_TRUE(stopped.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(indexer_ffi_stop_calls(), 1);
    events = lifecycleEvents();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("stopped"));

    const fs::path store = temp_dir.path / ("rocksdb-" + kChannelId);
    fs::create_directories(store);
    std::ofstream(store / "marker") << "test";
    LOGOS_ASSERT_TRUE(fs::exists(store));

    reset_node_changed_events();
    const json reset = nodeAction(module, lifecycleCommand("indexer-reset-v1", "reset"));
    LOGOS_ASSERT_TRUE(reset.at("accepted").get<bool>());
    events = lifecycleEvents();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("succeeded"));
    LOGOS_ASSERT_EQ(events.at(1).at("status").at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_FALSE(fs::exists(store));
    status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("epoch").get<std::uint64_t>(), 2U);
    LOGOS_ASSERT_EQ(status.at("scope").at("channel_id").get<std::string>(), kChannelId);
}

LOGOS_TEST(node_action_rejects_stale_requests_and_deduplicates_without_ffi_dispatch) {
    reset_indexer_ffi_mock();
    TempDir temp_dir;
    LOGOS_ASSERT_TRUE(temp_dir.valid());
    const std::string config_path = writeIndexerConfig(temp_dir);
    LezIndexerModuleImpl module;
    setTestContext(module, temp_dir);

    const json start = lifecycleCommand("indexer-reused-operation-v1", "start", {{"config_path", config_path}});
    LOGOS_ASSERT_TRUE(nodeAction(module, start).at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(indexer_ffi_start_calls(), 1);

    reset_node_changed_events();
    const json duplicate = nodeAction(module, start);
    LOGOS_ASSERT_TRUE(duplicate.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(duplicate.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(indexer_ffi_start_calls(), 1);
    LOGOS_ASSERT_EQ(lifecycleEvents().size(), static_cast<size_t>(0));

    const json conflict = nodeAction(module, lifecycleCommand("indexer-reused-operation-v1", "stop"));
    LOGOS_ASSERT_FALSE(conflict.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(conflict.at("error").at("code").get<std::string>(), std::string("operation_id_conflict"));
    LOGOS_ASSERT_EQ(indexer_ffi_stop_calls(), 0);

    const json current = nodeStatus(module);
    json stale = lifecycleCommand("indexer-stale-stop-v1", "stop");
    stale["expected"] = {
        {"instance_id", current.at("instance_id")},
        {"epoch", current.at("epoch")},
        {"sequence", current.at("sequence").get<std::uint64_t>() + 1},
    };
    reset_node_changed_events();
    const json stale_acknowledgement = nodeAction(module, stale);
    LOGOS_ASSERT_FALSE(stale_acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(stale_acknowledgement.at("error").at("code").get<std::string>(), std::string("state_mismatch"));
    LOGOS_ASSERT_EQ(indexer_ffi_stop_calls(), 0);
    const std::vector<json> events = lifecycleEvents();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(events.at(0).at("outcome").get<std::string>(), std::string("rejected"));

    LOGOS_ASSERT_TRUE(nodeAction(module, lifecycleCommand("indexer-cleanup-v1", "stop")).at("accepted").get<bool>());
}

LOGOS_TEST(node_action_reports_safe_start_failures_without_host_path_leaks) {
    reset_indexer_ffi_mock();
    set_indexer_ffi_start_status(InitializationError);
    TempDir temp_dir;
    LOGOS_ASSERT_TRUE(temp_dir.valid());
    const std::string config_path = writeIndexerConfig(temp_dir, "never-expose-host-path.json");
    LezIndexerModuleImpl module;
    setTestContext(module, temp_dir);

    reset_node_changed_events();
    const json acknowledgement =
        nodeAction(module, lifecycleCommand("indexer-failing-start-v1", "start", {{"config_path", config_path}}));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    const std::vector<json> events = lifecycleEvents();
    LOGOS_ASSERT_EQ(events.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(events.at(1).at("outcome").get<std::string>(), std::string("failed"));
    LOGOS_ASSERT_EQ(events.at(1).at("error").at("code").get<std::string>(), std::string("start_failed"));
    LOGOS_ASSERT_TRUE(events.at(1).dump().find("never-expose-host-path") == std::string::npos);
    const json status = nodeStatus(module);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("uninitialized"));
    LOGOS_ASSERT_EQ(status.at("health").get<std::string>(), std::string("degraded"));
    LOGOS_ASSERT_TRUE(status.dump().find("never-expose-host-path") == std::string::npos);
}

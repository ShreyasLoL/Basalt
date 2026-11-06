#include "basalt/persistence/catalog.hpp"
#include "basalt/persistence/checkpoint.hpp"
#include "basalt/persistence/event_log.hpp"
#include "basalt/persistence/history.hpp"
#include "basalt/persistence/replay.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace basalt::persistence {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::current_path() / "phase4-test-temp" /
                ("basalt-phase4-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

TEST(EventLogTest, RequiresRecoveryAndReopensAppendOnlyFrames) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "events.blog";
    EventLog log(path);
    EXPECT_EQ(log.append(1, "one").error().code, "LOG_NOT_RECOVERED");
    ASSERT_TRUE(log.recover());
    ASSERT_TRUE(log.append(1, "one"));
    ASSERT_TRUE(log.append(3, "three"));
    EXPECT_EQ(log.append(3, "duplicate").error().code, "LOG_SEQUENCE");

    EventLog reopened(path);
    const auto report = reopened.recover();
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().valid_frames, 2U);
    EXPECT_EQ(report.value().last_sequence, 3U);
    const auto frames = reopened.read_all();
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 2U);
    EXPECT_EQ(frames.value()[0], (LogFrame{1, "one"}));
    EXPECT_EQ(frames.value()[1], (LogFrame{3, "three"}));
}

TEST(EventLogTest, QuarantinesTornTailAndContinuesAtLastValidSequence) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "events.blog";
    EventLog writer(path);
    ASSERT_TRUE(writer.recover());
    ASSERT_TRUE(writer.append(1, "first"));
    {
        std::ofstream damaged(path, std::ios::binary | std::ios::app);
        damaged.write("BLG1\x01", 5);
    }

    EventLog recovered(path);
    const auto report = recovered.recover();
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().valid_frames, 1U);
    EXPECT_EQ(report.value().discarded_bytes, 5U);
    ASSERT_FALSE(report.value().quarantined_tail.empty());
    EXPECT_EQ(std::filesystem::file_size(report.value().quarantined_tail), 5U);
    EXPECT_EQ(std::filesystem::file_size(path), report.value().valid_bytes);
    ASSERT_TRUE(recovered.append(2, "second"));
    const auto frames = recovered.read_all();
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 2U);
    EXPECT_EQ(frames.value().back().sequence, 2U);
}

TEST(EventLogTest, VerifiesCrcAndRecoversOnlyThePriorCompletePrefix) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "events.blog";
    EventLog writer(path);
    ASSERT_TRUE(writer.recover());
    ASSERT_TRUE(writer.append(1, "one"));
    ASSERT_TRUE(writer.append(2, "two"));
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(22 + 3 + 22 + 1, std::ios::beg);
        const char damaged = 'X';
        file.write(&damaged, 1);
    }
    EXPECT_EQ(writer.read_all().error().code, "LOG_CORRUPT");

    EventLog recovered(path);
    const auto report = recovered.recover();
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().valid_frames, 1U);
    EXPECT_EQ(report.value().last_sequence, 1U);
    EXPECT_GT(report.value().discarded_bytes, 0U);
    EXPECT_EQ(std::filesystem::file_size(path), report.value().valid_bytes);
    const auto frames = recovered.read_all();
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 1U);
    EXPECT_EQ(frames.value()[0].payload, "one");
}

TEST(EventLogTest, RejectsOversizedPayloadAndUsesStandardCrc32) {
    const std::string sample = "123456789";
    EXPECT_EQ(EventLog::crc32(reinterpret_cast<const std::byte*>(sample.data()), sample.size()), 0xcbf43926U);
    TemporaryDirectory directory;
    EventLog log(directory.path() / "events.blog", 4U);
    ASSERT_TRUE(log.recover());
    EXPECT_EQ(log.append(1, "12345").error().code, "LOG_FRAME_TOO_LARGE");
}

TEST(ReplayTest, CheckpointPlusLogProducesRepeatableStateAndEventManifest) {
    TemporaryDirectory directory;
    EventLog log(directory.path() / "events.blog");
    ASSERT_TRUE(log.recover());
    ASSERT_TRUE(log.append(1, "ignored-before-snapshot"));
    ASSERT_TRUE(log.append(2, "fill:10000:2"));
    ASSERT_TRUE(log.append(3, "mark:10500"));
    const auto frames = log.read_all();
    ASSERT_TRUE(frames);

    Checkpoint checkpoint{1, HistoryStore::sha256("position=0"), "position=0"};
    const auto apply = [](std::string& state, const LogFrame& frame) {
        state += "|" + frame.payload;
        return common::Result<void>::success();
    };
    const auto hash = [](const std::string_view state) { return HistoryStore::sha256(state); };
    const auto first = replay_from_checkpoint(checkpoint, frames.value(), apply, hash);
    const auto second = replay_from_checkpoint(checkpoint, frames.value(), apply, hash);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(first.value().state_blob, "position=0|fill:10000:2|mark:10500");
    EXPECT_EQ(first.value().manifest.start_sequence, 1U);
    EXPECT_EQ(first.value().manifest.end_sequence, 3U);
    EXPECT_EQ(first.value().manifest.replayed_events, 2U);
    EXPECT_EQ(first.value().manifest.event_sha256.size(), 64U);
    EXPECT_EQ(first.value().manifest.state_sha256, HistoryStore::sha256(first.value().state_blob));

    checkpoint.state_hash = "wrong-hash";
    EXPECT_EQ(replay_from_checkpoint(checkpoint, frames.value(), apply, hash).error().code,
              "REPLAY_CHECKPOINT_HASH");
}

TEST(CheckpointTest, AtomicallyPersistsStateAndDetectsCorruption) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "state.checkpoint";
    CheckpointStore store(path);
    const auto first_save = store.save({42, "state-hash", "authoritative-state"});
    ASSERT_TRUE(first_save) << first_save.error().code << ": " << first_save.error().message;
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.value(), (Checkpoint{42, "state-hash", "authoritative-state"}));

    ASSERT_TRUE(store.save({43, "next-hash", "next-state"}));
    EXPECT_EQ(store.load().value().event_sequence, 43U);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(-1, std::ios::end);
        const char damaged = 'X';
        file.write(&damaged, 1);
    }
    EXPECT_EQ(store.load().error().code, "CHECKPOINT_CRC");
}

TEST(CatalogTest, ReopensDeterministicTransactionalDatasetMetadata) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "catalog.json";
    DatasetRecord first{"z-last", 12, "csv-int64-ohlcv-v1", "fixture", "z-last.csv", "hash-z", 10, 20, 30, "test-only", 60};
    DatasetRecord second{"a-first", 5, "csv-int64-ohlcv-v1", "fixture", "a-first.csv", "hash-a", 1, 2, 3, "test-only", 60};
    {
        Catalog catalog(path);
        const auto opened = catalog.open();
        ASSERT_TRUE(opened) << opened.error().code << ": " << opened.error().message;
        const auto first_written = catalog.upsert_dataset(first);
        ASSERT_TRUE(first_written) << first_written.error().code << ": " << first_written.error().message;
        const auto second_written = catalog.upsert_dataset(second);
        ASSERT_TRUE(second_written) << second_written.error().code << ": " << second_written.error().message;
    }
    {
        Catalog reopened(path);
        ASSERT_TRUE(reopened.open());
        const auto records = reopened.datasets();
        ASSERT_TRUE(records);
        ASSERT_EQ(records.value().size(), 2U);
        EXPECT_EQ(records.value()[0].dataset_id, "a-first");
        EXPECT_EQ(reopened.dataset("z-last").value().value(), first);
        EXPECT_FALSE(reopened.dataset("missing").value().has_value());
    }
}

TEST(CatalogTest, AppliesAndRecordsVersionZeroToOneMigration) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "legacy-catalog.json";
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"schema_version":0,"datasets":[{"dataset_id":"old","format":"csv","source":"fixture","path":"old.csv","sha256":"abc","imported_at_ns":9}]})";
    }
    Catalog catalog(path);
    ASSERT_TRUE(catalog.open());
    const auto record = catalog.dataset("old");
    ASSERT_TRUE(record);
    ASSERT_TRUE(record.value());
    EXPECT_EQ(record.value()->license_tag, "unspecified");
    EXPECT_EQ(record.value()->interval_ns, 0U);
    std::ifstream migrated_file(path, std::ios::binary);
    const std::string migrated((std::istreambuf_iterator<char>(migrated_file)), {});
    EXPECT_NE(migrated.find("\"schema_version\": 1"), std::string::npos);
    EXPECT_NE(migrated.find("dataset-catalog-v1"), std::string::npos);
    EXPECT_NE(migrated.find("\"checksum\""), std::string::npos);
}

TEST(HistoryTest, ImportsVerifiesAndExportsIntegerBarsWithSha256) {
    EXPECT_EQ(HistoryStore::sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    TemporaryDirectory directory;
    const auto input = directory.path() / "source.csv";
    {
        std::ofstream file(input, std::ios::binary);
        file << "open_time_ns,open_micro,high_micro,low_micro,close_micro,volume\n"
                "100,10000,11000,9000,10500,12\n"
                "200,10500,12000,10000,11500,20\n";
    }
    Catalog catalog(directory.path() / "catalog.json");
    ASSERT_TRUE(catalog.open());
    HistoryStore history(directory.path() / "history", catalog);
    const DatasetImport metadata{"fixture-1", 7, 60'000'000'000ULL, "local-test-fixture", 300, "test-only"};
    const auto imported = history.import_csv(input, metadata);
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().range_start_ns.value(), 100);
    EXPECT_EQ(imported.value().range_end_ns.value(), 200);
    EXPECT_EQ(imported.value().sha256.size(), 64U);
    const auto bars = history.read_bars("fixture-1");
    ASSERT_TRUE(bars);
    ASSERT_EQ(bars.value().size(), 2U);
    EXPECT_EQ(bars.value()[1].close_micro, 11500);

    const auto exported_path = directory.path() / "export.csv";
    ASSERT_TRUE(history.export_csv("fixture-1", exported_path));
    std::ifstream exported_file(exported_path, std::ios::binary);
    const std::string exported((std::istreambuf_iterator<char>(exported_file)), {});
    EXPECT_EQ(HistoryStore::sha256(exported), imported.value().sha256);

    Catalog reopened_catalog(directory.path() / "catalog.json");
    ASSERT_TRUE(reopened_catalog.open());
    HistoryStore reopened_history(directory.path() / "history", reopened_catalog);
    EXPECT_EQ(reopened_history.read_bars("fixture-1").value(), bars.value());
    {
        std::ofstream corrupt(directory.path() / "history" / imported.value().path, std::ios::binary | std::ios::app);
        corrupt << "tampered";
    }
    EXPECT_EQ(reopened_history.read_bars("fixture-1").error().code, "DATASET_HASH");
}

TEST(HistoryTest, RejectsUnorderedInvalidAndPathLikeDatasetInputs) {
    TemporaryDirectory directory;
    const auto input = directory.path() / "invalid.csv";
    {
        std::ofstream file(input);
        file << "open_time_ns,open_micro,high_micro,low_micro,close_micro,volume\n"
                "2,100,90,80,85,1\n";
    }
    Catalog catalog(directory.path() / "catalog.json");
    ASSERT_TRUE(catalog.open());
    HistoryStore history(directory.path() / "history", catalog);
    EXPECT_EQ(history.import_csv(input, {"../escape", 1, 60, "fixture", 0, "test"}).error().code, "DATASET_METADATA");
    EXPECT_EQ(history.import_csv(input, {"valid-id", 1, 60, "fixture", 0, "test"}).error().code, "DATASET_OHLC");
}

} // namespace
} // namespace basalt::persistence

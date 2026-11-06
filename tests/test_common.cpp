#include "basalt/common/config.hpp"
#include "basalt/common/logger.hpp"
#include "basalt/common/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace {

using namespace basalt::common;

TEST(CommonTypes, FixedPointScaleAndCheckedAddition) {
    EXPECT_EQ(PRICE_SCALE, 10'000);
    const auto sum = checked_add(10'000, 2'500);
    ASSERT_TRUE(sum);
    EXPECT_EQ(sum.value(), 12'500);

    const auto overflow = checked_add(std::numeric_limits<std::int64_t>::max(), 1);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, "ARITHMETIC_OVERFLOW");
}

TEST(CommonTypes, CheckedMultiplicationHandlesSignsAndOverflow) {
    const auto product = checked_multiply(-12'500, 4);
    ASSERT_TRUE(product);
    EXPECT_EQ(product.value(), -50'000);

    const auto overflow = checked_multiply(std::numeric_limits<std::int64_t>::min(), -1);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, "ARITHMETIC_OVERFLOW");
}

TEST(CommonTypes, ResultSupportsValueAndVoidErrors) {
    const auto result = Result<int>::success(42);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 42);

    const auto failure = Result<void>::failure(Error{"BAD", "invalid"});
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().code, "BAD");
}

TEST(Config, DefaultsAreLoopbackAndValidated) {
    const auto config = ConfigLoader::parse({});
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().bind_address, "127.0.0.1");
    EXPECT_EQ(config.value().port, 8080U);
    EXPECT_FALSE(config.value().allow_remote_bind);
    EXPECT_EQ(config.value().event_queue_capacity, 65'536U);
}

TEST(Config, RejectsUnsafeOrOutOfRangeValues) {
    auto remote = ConfigLoader::parse({{"BASALT_BIND_ADDRESS", "0.0.0.0"}});
    ASSERT_FALSE(remote);
    EXPECT_EQ(remote.error().code, "REMOTE_BIND_DISABLED");

    auto bad_port = ConfigLoader::parse({{"BASALT_PORT", "65536"}});
    ASSERT_FALSE(bad_port);
    EXPECT_EQ(bad_port.error().code, "INVALID_PORT");

    auto bad_capacity = ConfigLoader::parse({{"BASALT_EVENT_QUEUE_CAPACITY", "0"}});
    ASSERT_FALSE(bad_capacity);
    EXPECT_EQ(bad_capacity.error().code, "INVALID_QUEUE_CAPACITY");

    auto explicitly_remote = ConfigLoader::parse({
        {"BASALT_BIND_ADDRESS", "192.0.2.10"}, {"BASALT_ALLOW_REMOTE_BIND", "true"}});
    ASSERT_TRUE(explicitly_remote);
    EXPECT_EQ(explicitly_remote.value().bind_address, "192.0.2.10");
}

TEST(Config, ParsesAllowlistedSettingsAndRejectsUnknownEnum) {
    const auto config = ConfigLoader::parse({
        {"BASALT_PORT", "8081"}, {"BASALT_LOG_FORMAT", "console"},
        {"BASALT_UI_QUEUE_CAPACITY", "512"}, {"UNRELATED_SECRET", "must-not-be-read"}});
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().port, 8081U);
    EXPECT_EQ(config.value().log_output, LogOutput::Console);
    EXPECT_EQ(config.value().ui_queue_capacity, 512U);

    const auto invalid = ConfigLoader::parse({{"BASALT_LOG_FORMAT", "xml"}});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, "INVALID_LOG_FORMAT");
}

TEST(Logger, RedactsSensitiveFieldsAndEmbeddedAssignments) {
    std::ostringstream output;
    Logger logger(output, LogFormat::Json, LogLevel::Debug);
    logger.log(LogLevel::Info, "provider_connected", {
        {"provider", "offline"}, {"access_token", "top-secret-token"},
        {"message", "handshake api_key=hidden-key; state=ready"}});
    const std::string record = output.str();

    EXPECT_NE(record.find("provider_connected"), std::string::npos);
    EXPECT_NE(record.find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(record.find("top-secret-token"), std::string::npos);
    EXPECT_EQ(record.find("hidden-key"), std::string::npos);
    EXPECT_NE(record.find("state=ready"), std::string::npos);
    EXPECT_EQ(record.back(), '\n');
}

TEST(Logger, ConsoleFormatRedactsAndHonorsMinimumLevel) {
    std::ostringstream output;
    Logger logger(output, LogFormat::Console, LogLevel::Warning);
    logger.log(LogLevel::Info, "filtered", {{"token", "not-written"}});
    logger.log(LogLevel::Warning, "auth", {{"password", "not-written-either"}});
    const std::string record = output.str();
    EXPECT_EQ(record.find("filtered"), std::string::npos);
    EXPECT_NE(record.find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(record.find("not-written"), std::string::npos);
}

} // namespace

cmake_minimum_required(VERSION 3.20)
project(BASALT VERSION 1.0.0 LANGUAGES CXX)

set(BASALT_GIT_REVISION "unknown")
set(BASALT_GIT_WORKTREE_DIRTY "true")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE BASALT_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE BASALT_GIT_REVISION_RESULT)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE BASALT_GIT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE BASALT_GIT_STATUS_RESULT)
    if(NOT BASALT_GIT_REVISION_RESULT EQUAL 0)
        set(BASALT_GIT_REVISION "unknown")
    endif()
    if(BASALT_GIT_STATUS_RESULT EQUAL 0)
        if(BASALT_GIT_STATUS STREQUAL "")
            set(BASALT_GIT_WORKTREE_DIRTY "false")
        else()
            set(BASALT_GIT_WORKTREE_DIRTY "true")
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BASALT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(basalt_enable_sanitizers target)
    if(NOT BASALT_ENABLE_SANITIZERS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "BASALT_ENABLE_SANITIZERS is unsupported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endfunction()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
if(WIN32 AND MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
endif()

# Dependency declarations are shared with the later test and server phases.
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(googletest)
FetchContent_MakeAvailable(httplib nlohmann_json)
if(NOT MSVC)
    # GoogleTest enables extra diagnostics that report warnings in its own upstream source.
    target_compile_options(gtest PRIVATE -Wno-undef -Wno-character-conversion)
    target_compile_options(gtest_main PRIVATE -Wno-undef -Wno-character-conversion)
    target_compile_options(gmock PRIVATE -Wno-undef -Wno-character-conversion)
    target_compile_options(gmock_main PRIVATE -Wno-undef -Wno-character-conversion)
endif()

add_library(basalt_core STATIC
    src/core/types.cpp
    src/core/order_book.cpp
    src/core/matching_engine.cpp
    src/core/multi_instrument_engine.cpp
    src/core/arena_order_book.cpp
    src/core/vector_order_book.cpp
    src/bench/workload_generator.cpp
    src/bench/benchmark_runner.cpp
)
target_include_directories(basalt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(basalt_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_core PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_core)

add_library(basalt_common INTERFACE)
target_include_directories(basalt_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(basalt_common INTERFACE cxx_std_20)

add_library(basalt_market_data STATIC
    src/market_data/types.cpp
    src/market_data/instrument_master.cpp
    src/market_data/event_bus.cpp
    src/market_data/pipeline.cpp
    src/market_data/providers.cpp
)
target_include_directories(basalt_market_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_market_data PUBLIC basalt_common PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(basalt_market_data PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_market_data PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_market_data PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_market_data)
target_link_libraries(basalt_core PRIVATE basalt_market_data)

add_library(basalt_persistence STATIC
    src/persistence/storage_io.cpp
    src/persistence/event_log.cpp
    src/persistence/checkpoint.cpp
    src/persistence/replay.cpp
    src/persistence/catalog.cpp
    src/persistence/history.cpp
)
target_include_directories(basalt_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_persistence PUBLIC basalt_common PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(basalt_persistence PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_persistence PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_persistence PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_persistence)

add_library(basalt_trading STATIC
    src/trading/risk.cpp
    src/trading/execution.cpp
    src/trading/ledger.cpp
)
target_include_directories(basalt_trading PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_trading PUBLIC basalt_common basalt_market_data basalt_persistence basalt_core PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(basalt_trading PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_trading PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_trading PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_trading)

add_library(basalt_strategy STATIC
    src/strategy/indicators.cpp
    src/strategy/runtime.cpp
    src/strategy/builtins.cpp
)
target_include_directories(basalt_strategy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_strategy PUBLIC basalt_common basalt_market_data basalt_trading)
target_compile_features(basalt_strategy PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_strategy PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_strategy PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_strategy)

add_library(basalt_backtest STATIC
    src/backtest/backtest.cpp
    src/backtest/analytics.cpp
    src/backtest/replay.cpp
)
target_include_directories(basalt_backtest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_backtest PUBLIC basalt_common basalt_strategy basalt_trading basalt_persistence
                      PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(basalt_backtest PRIVATE
    BASALT_GIT_REVISION="${BASALT_GIT_REVISION}"
    BASALT_GIT_WORKTREE_DIRTY=${BASALT_GIT_WORKTREE_DIRTY})
target_compile_features(basalt_backtest PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_backtest PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_backtest PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_backtest)

enable_testing()
add_executable(basalt_tests tests/test_matching_engine.cpp)
target_link_libraries(basalt_tests PRIVATE basalt_core GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
include(GoogleTest)
gtest_discover_tests(basalt_tests)
basalt_enable_sanitizers(basalt_tests)

add_executable(basalt_common_tests tests/test_common.cpp)
target_link_libraries(basalt_common_tests PRIVATE basalt_common GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_common_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_common_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_common_tests)
basalt_enable_sanitizers(basalt_common_tests)

add_executable(basalt_market_data_tests tests/test_market_data.cpp)
target_link_libraries(basalt_market_data_tests PRIVATE basalt_market_data GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_market_data_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_market_data_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_market_data_tests)
basalt_enable_sanitizers(basalt_market_data_tests)

add_executable(basalt_persistence_tests tests/test_persistence.cpp)
target_link_libraries(basalt_persistence_tests PRIVATE basalt_persistence GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_persistence_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_persistence_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_persistence_tests)
basalt_enable_sanitizers(basalt_persistence_tests)

add_executable(basalt_trading_tests tests/test_trading.cpp)
target_link_libraries(basalt_trading_tests PRIVATE basalt_trading GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_trading_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_trading_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_trading_tests)
basalt_enable_sanitizers(basalt_trading_tests)

add_executable(basalt_strategy_tests tests/test_strategy.cpp)
target_link_libraries(basalt_strategy_tests PRIVATE basalt_strategy GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_strategy_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_strategy_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_strategy_tests)
basalt_enable_sanitizers(basalt_strategy_tests)

add_executable(basalt_multi_instrument_tests tests/test_multi_instrument_engine.cpp)
target_link_libraries(basalt_multi_instrument_tests PRIVATE basalt_core GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_multi_instrument_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_multi_instrument_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_multi_instrument_tests)
basalt_enable_sanitizers(basalt_multi_instrument_tests)

add_executable(basalt_bench_tests tests/test_bench.cpp)
target_link_libraries(basalt_bench_tests PRIVATE basalt_core GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_bench_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_bench_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_bench_tests)
basalt_enable_sanitizers(basalt_bench_tests)

add_executable(basalt_backtest_tests tests/test_backtest.cpp)
target_link_libraries(basalt_backtest_tests PRIVATE basalt_backtest GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_backtest_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_backtest_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_backtest_tests)
basalt_enable_sanitizers(basalt_backtest_tests)

add_library(basalt_app STATIC src/server/simulation_facade.cpp)
target_include_directories(basalt_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_app PUBLIC basalt_core)
target_compile_features(basalt_app PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_app PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_app PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_app)

add_library(basalt_mcp STATIC src/server/mcp_server.cpp)
target_include_directories(basalt_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(basalt_mcp PUBLIC basalt_app basalt_backtest PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(basalt_mcp PRIVATE
    BASALT_GIT_REVISION="${BASALT_GIT_REVISION}"
    BASALT_GIT_WORKTREE_DIRTY=${BASALT_GIT_WORKTREE_DIRTY})
target_compile_features(basalt_mcp PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(basalt_mcp PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_mcp PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_mcp)

add_executable(basalt_mcp_tests tests/test_mcp.cpp)
target_link_libraries(basalt_mcp_tests PRIVATE basalt_mcp nlohmann_json::nlohmann_json GTest::gtest_main)
if(MSVC)
    target_compile_options(basalt_mcp_tests PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_mcp_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
gtest_discover_tests(basalt_mcp_tests)
basalt_enable_sanitizers(basalt_mcp_tests)

add_executable(basalt_bench src/bench/main_bench.cpp)
target_link_libraries(basalt_bench PRIVATE basalt_core)
if(MSVC)
    target_compile_options(basalt_bench PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_bench)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" BASALT_WEB_HTML)
configure_file(src/server/embedded_ui.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/basalt_embedded_ui.hpp" @ONLY)

add_executable(basalt_server src/server/http_server.cpp src/server/main_server.cpp)
target_include_directories(basalt_server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(basalt_server PRIVATE
    BASALT_FRONTEND_DIST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web/app"
    BASALT_GIT_REVISION="${BASALT_GIT_REVISION}"
    BASALT_GIT_WORKTREE_DIRTY=${BASALT_GIT_WORKTREE_DIRTY})
target_link_libraries(basalt_server PRIVATE basalt_app basalt_mcp httplib::httplib nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(basalt_server PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(basalt_server PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_server PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
basalt_enable_sanitizers(basalt_server)


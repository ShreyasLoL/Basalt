cmake_minimum_required(VERSION 3.20)
project(BASALT VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_executable(basalt_bench src/bench/main_bench.cpp)
target_link_libraries(basalt_bench PRIVATE basalt_core)
if(MSVC)
    target_compile_options(basalt_bench PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" BASALT_WEB_HTML)
configure_file(src/server/embedded_ui.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/basalt_embedded_ui.hpp" @ONLY)

add_executable(basalt_server src/server/http_server.cpp src/server/main_server.cpp)
target_include_directories(basalt_server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(basalt_server PRIVATE basalt_core httplib::httplib nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(basalt_server PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(basalt_server PRIVATE /W4 /WX)
else()
    target_compile_options(basalt_server PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()


cmake_minimum_required(VERSION 3.20)
project(ProjectAtlas VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
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

add_library(atlas_core STATIC src/core/types.cpp)
target_include_directories(atlas_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(atlas_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(atlas_core PRIVATE /W4 /WX)
else()
    target_compile_options(atlas_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(atlas_phase1_smoke src/phase1_smoke.cpp)
target_link_libraries(atlas_phase1_smoke PRIVATE atlas_core)


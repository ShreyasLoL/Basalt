# Fuzzing and sanitizer findings

## Harnesses and corpora

`BASALT_ENABLE_FUZZERS=ON` adds four Clang/libFuzzer entry points:

| Target | Production code exercised | Seed corpus |
|---|---|---|
| `basalt_fuzz_event_log` | `.blog` framing, recovery and valid-prefix reading | empty log, truncated header, invalid frame |
| `basalt_fuzz_csv` | `HistoryStore::parse_csv` | valid row, malformed row |
| `basalt_fuzz_json` | nlohmann JSON parser | valid object, truncated array |
| `basalt_fuzz_mcp` | `McpServer::process_line` request-frame parser | initialize request, malformed frame |

Each harness rejects inputs above a bounded size. Project objects are compiled with libFuzzer coverage instrumentation and ASan/UBSan; harness executables link the libFuzzer runtime. The Linux CI job assigns a 30-second budget to each target. A crashed input should be retained in the corresponding corpus and covered by a deterministic regression test.

## Local verification

Host: Windows 11, Clang 22.1.8 (`x86_64-pc-windows-msvc`), Ninja, Release.

- `cmake --build build-release --clean-first --parallel` — passed, 81/81 targets, no compiler warnings.
- `ctest --test-dir build-release --output-on-failure` — passed, 157/157 in 7.77 seconds after the final clean build; loopback HTTP tests ran with loopback access enabled.
- `cmake --build build-asan --clean-first --parallel` — passed, 81/81 targets with ASan+UBSan. The Clang runtime directory was added to `PATH` for Windows DLL discovery.
- `$env:PATH = 'C:\Program Files\LLVM\lib\clang\22\lib\windows;' + $env:PATH; ctest --test-dir build-asan --output-on-failure` — passed, 157/157 in 24.90 seconds, with no sanitizer report.
- `npm run typecheck` — passed.
- `npm test -- --reporter=dot` — passed, 10/10 across 3 files.
- `npm run build` — passed, 42 modules transformed.

TSan was not run locally. Windows Clang does not provide the supported TSan runtime used by this project; configuring `-DBASALT_ENABLE_TSAN=ON` fails early with that platform requirement. `wsl.exe --status` returned `Wsl/E_ACCESSDENIED`, so a Linux environment could not be used from this host. A Linux Clang TSan job is defined in `.github/workflows/ci.yml`, but has not run because this local branch has not been pushed.

The four harness translation units compiled in the earlier Windows libFuzzer attempt, but linking the prebuilt Windows runtime failed with an `annotate_string` mismatch (`FuzzerCrossOver.cpp.obj` value 0 versus the ASan-instrumented harness value 1). The CMake option now fails early on Windows rather than implying that this runtime is supported. The Linux Clang CI fuzz job builds the harnesses and runs their corpora for 30 seconds each, but that job has not run from this local branch. **No fuzz campaign result is claimed; crash/hang discovery remains deferred until a compatible Linux run is available.**

## Randomized order-book parity

`OrderBookPropertyTest.SeededRandomSequencesPreserveParityPriorityAndConservation` runs four deterministic workloads (balanced, aggressive matching, cancellation-heavy and wide price distribution) with seeds 17 and 2026, 750 operations per profile/seed, against map/list, vector and arena implementations. It compares operation status, trade records and full snapshots after each operation; verifies each book's invariants, resting-price execution, limit eligibility, price/time ordering and per-order quantity conservation. The test passed in both the normal and ASan/UBSan suites.

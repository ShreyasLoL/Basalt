# Security Testing

These commands are the reproducible security-oriented checks configured for BASALT. Run them from the repository root unless noted. Scanner jobs require network access; sanitizer, TSan and libFuzzer configurations use a supported Linux Clang/GCC environment. TSan and libFuzzer are not supported by the current Windows CMake configuration.

## Build and regression tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Frontend checks:

```sh
cd frontend
npm ci
npm run typecheck
npm test -- --reporter=dot
npm run build
npm audit --package-lock-only --audit-level=low
cd ..
```

`npm audit` needs access to the npm registry. It reads the lockfile without requiring a separate install in the audit-only invocation.

## Native dependency pins and CycloneDX inventory

The checked-in CMake pin lock records the exact Git repository and 40-character commit expected for each FetchContent dependency. Verify the lock against `CMakeLists.txt` and confirm that the committed CycloneDX 1.5 BOM matches both that lock and the complete frontend npm lockfile:

```sh
python scripts/dependency_sbom.py --check
```

After an intentional dependency change, review and update `dependencies/cmake-fetchcontent.lock.json` or `frontend/package-lock.json`, then regenerate `sbom/basalt.cdx.json` with `python scripts/dependency_sbom.py`. The BOM inventories three immutable C++ source dependencies and every package-lock entry, including transitive npm packages. This inventory is not a vulnerability scan or source-integrity attestation.

## C++ static analysis

The CI clang-tidy profile analyzes every `src/**/*.cpp` translation unit with the Clang static analyzer's core and C++ checks and treats unsuppressed warnings as errors:

```sh
cmake -S . -B build-clang-tidy -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
find src -type f -name '*.cpp' -print0 |
  xargs -0 -r clang-tidy -p build-clang-tidy \
    --checks='-*,clang-analyzer-core.*,clang-analyzer-cplusplus.*,-clang-analyzer-core.StackAddressEscape' \
    --warnings-as-errors='*'
```

`clang-analyzer-core.StackAddressEscape` is excluded because it flags the pinned cpp-httplib callback wrapper's internally owned handler when instantiated; focused inspection found no application-owned escaping stack reference. This narrow exclusion is documented in the workflow and audit. Other enabled warnings fail the job.

## Sanitizers

ASan and UBSan run together in a separate build:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBASALT_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

ThreadSanitizer uses its own build and cannot be combined with ASan/UBSan:

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBASALT_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

## Bounded fuzz smoke tests

With Clang/libFuzzer on Linux:

```sh
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBASALT_ENABLE_FUZZERS=ON
cmake --build build-fuzz --parallel
./build-fuzz/basalt_fuzz_event_log -max_total_time=30 -max_len=65536 fuzz/corpus/event_log
./build-fuzz/basalt_fuzz_csv -max_total_time=30 -max_len=262144 fuzz/corpus/csv
./build-fuzz/basalt_fuzz_jsonl -max_total_time=30 -max_len=262144 fuzz/corpus/jsonl
./build-fuzz/basalt_fuzz_order_book -max_total_time=30 -max_len=6912 fuzz/corpus/order_book
./build-fuzz/basalt_fuzz_http_validation -max_total_time=30 -max_len=65537 fuzz/corpus/http_validation
./build-fuzz/basalt_fuzz_json -max_total_time=30 -max_len=262144 fuzz/corpus/json
./build-fuzz/basalt_fuzz_mcp -max_total_time=30 -max_len=307200 fuzz/corpus/mcp
```

These are bounded smoke campaigns for EventLog recovery, CSV/JSONL parsing, order-book state transitions, HTTP mutation schemas, JSON and MCP frames. They are not a claim of exhaustive fuzz coverage.

## Dependency and secret scans

CI verifies the FetchContent pin lock and CycloneDX BOM, runs `npm audit`, and uses read-only permissions and pinned container digests for OSV Scanner and Gitleaks. The SBOM is committed at `sbom/basalt.cdx.json`. For a manual scan with Docker installed and a complete Git history checked out:

```sh
docker run --rm -v "$PWD:/src" -w /src \
  ghcr.io/google/osv-scanner:v2.7.1@sha256:a70ffffc2bfd33482953e5d6b64c6fcdb30239717a07b0f0d3ab2859ce3e4cde \
  scan source --recursive /src

docker run --rm -v "$PWD:/src" -w /src \
  ghcr.io/gitleaks/gitleaks:v8.30.1@sha256:c00b6bd0aeb3071cbcb79009cb16a60dd9e0a7c60e2be9ab65d25e6bc8abbb7f \
  git --redact=100 --no-banner --log-opts=--all /src
```

Gitleaks redacts detected values in output and scans reachable history. Review findings without copying secret values into tickets or logs. Do not add broad ignore rules; suppress only a reviewed fingerprint when it is confirmed to be a non-secret fixture.

## Current local verification limits

On 2026-11-25, the full Release CTest suite passed **177/177**. A separate Clang 22.1.8 ASan/UBSan build also passed CTest **177/177**; on this Windows host, Clang's `lib/clang/<version>/lib/windows` directory had to be on `PATH` so the sanitizer DLL could load. Repo-wide clang-tidy completed across 36 backend translation units with exit code 0 using the profile above. `actionlint` 1.7.12 parsed the workflow successfully. Frontend typecheck passed, Vitest passed **10/10**, and the production build passed. `npm audit --package-lock-only --audit-level=low` reported zero vulnerabilities. OSV Scanner v2.7.1 reported no issues in the 236 packages resolved from `frontend/package-lock.json`; its observed output did not include the CMake FetchContent commits.

Gitleaks' initial history scan covered 51 commits and its most recent history scan, run after `11df918`, covered 53; neither found leaks. Its working-tree scan reported eight redacted matches, all in generated `build-phase2/_deps/httplib-src` upstream documentation and test fixtures; it reported no findings elsewhere in the tree, including the local MCP config. These generated false positives were not added to a broad ignore file. The CI job scans full Git history from a clean checkout and does not configure or scan generated build directories.

This Windows host has no Docker executable, so the exact pinned container invocations were not run locally. The Gitleaks and OSV Scanner Windows release binaries were downloaded from official releases and verified against published SHA-256 digests before scanning. The hosted GitHub Actions workflow itself has not been triggered. CMake explicitly rejects TSan and libFuzzer configurations on Windows, so those runs were not performed locally; the existing CI jobs provide Linux configurations for them.

## Phase 3 local verification (2026-11-27)

- Full CTest: **182/182 passed** with loopback access. This verifies the newly added concurrency regression and storage/HTTP regressions along with the existing suite.
- Focused checks recorded with their feature commits: SimulationFacade concurrency 1/1; persistence fault injection/recovery 4/4; HTTP validation and replay/parser regressions 18/18.
- `python scripts/dependency_sbom.py --check` passed for the pinned CMake inputs and checked-in CycloneDX BOM. The local BOM contains 239 components: three native dependencies and 236 npm lockfile packages.
- Frontend typecheck passed; Vitest passed **16/16**; Vite production build passed. The updated accessibility and replay/backtest interaction tests use controlled API responses and do not claim that the current HTTP server implements those optional routes.
- A full build attempt compiled and linked targets until the final `basalt_server.exe` link. Windows returned permission denied while replacing the existing executable; no process was stopped. CTest was still run separately and passed. TSan and libFuzzer campaigns remain unrun on Windows, and hosted CI has not been observed.
- No browser visual/end-to-end QA is claimed. The available local preview path was previously unreachable, and the user requested no computer UI access during this turn.

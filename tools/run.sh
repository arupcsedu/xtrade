#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repository_root"

# shellcheck source=tools/toolchain.sh
source "$repository_root/tools/toolchain.sh"

venv_dir="${AEGIS_PYTHON_ENV:-$repository_root/.venv}"
python_bin="$venv_dir/bin/python"

print_help() {
  printf '%s\n' \
    'Aegis-MX foundation tasks:' \
    '  make bootstrap        Create the isolated environment and configure C++.' \
    '  make format           Apply C++, Python, and Go formatting.' \
    '  make format-check     Check formatting without changing files.' \
    '  make lint             Run clang-tidy, Ruff, mypy, gofmt, and go vet.' \
    '  make test             Run C++, Python, and Go unit tests.' \
    '  make test-sanitizers  Run ASan/UBSan, TSan, and the Go race detector.' \
    '  make test-fuzz        Run deterministic deserialization fuzz smoke tests.' \
    '  make benchmark        Run the C++ benchmark smoke workload.' \
    '  make package          Build C++, Python, control, and SBOM artifacts.' \
    '  make docs-check       Validate local documentation.' \
    '  make schemas-check    Verify generated bindings and golden schema files.' \
    '  make schemas-generate Regenerate bindings and golden schema files.' \
    '  make dependency-scan  Audit Python and Go dependencies and visible secrets.' \
    '  make fast             Run the pull-request validation set.' \
    '  make full             Run the exhaustive validation and packaging set.'
}

schema_flatc() {
  local schema_build_dir="$repository_root/build/schema-tools"
  local flatc_bin="$schema_build_dir/_deps/flatbuffers-build/flatc"
  cmake \
    -S "$repository_root" \
    -B "$schema_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DAEGIS_BUILD_TESTS=OFF \
    -DAEGIS_BUILD_BENCHMARKS=OFF \
    -DAEGIS_BUILD_SCHEMA_COMPILER=ON
  cmake --build "$schema_build_dir" --target flatc --parallel
  printf '%s\n' "$flatc_bin"
}

require_command() {
  local command_name=$1
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'Required command is unavailable: %s\n' "$command_name" >&2
    printf 'Use the pinned development container or install the documented prerequisite.\n' >&2
    exit 1
  fi
}

require_environment() {
  if [ ! -x "$python_bin" ]; then
    printf 'Python environment is missing. Run: make bootstrap\n' >&2
    exit 1
  fi
}

cpp_files() {
  find cpp -type f \( -name '*.cpp' -o -name '*.hpp' \) -print | sort
}

go_files() {
  find control -type f -name '*.go' -print | sort
}

bootstrap() {
  require_command cmake
  require_command ninja
  require_command "$CC"
  require_command "$CXX"
  require_command python3.12
  require_command go

  if [ ! -x "$python_bin" ]; then
    python3.12 -m venv "$venv_dir"
  fi
  "$python_bin" -m pip install \
    --require-hashes \
    --requirement python/requirements-dev.lock
  "$python_bin" -m pip install \
    --no-deps \
    --no-build-isolation \
    --editable .

  cmake --preset dev
  (
    cd control
    go mod download
  )

  printf '%s\n' \
    'Bootstrap complete.' \
    "Python: $($python_bin --version 2>&1)" \
    "CMake: $(cmake --version | sed -n '1p')" \
    "C++: $($CXX --version | sed -n '1p')" \
    "Go: $(go version)"
}

format_sources() {
  require_environment
  require_command clang-format
  mapfile -t cxx_sources < <(cpp_files)
  if [ "${#cxx_sources[@]}" -gt 0 ]; then
    clang-format -i "${cxx_sources[@]}"
  fi
  "$venv_dir/bin/ruff" format python tools
  "$venv_dir/bin/ruff" check --fix python tools
  mapfile -t go_sources < <(go_files)
  if [ "${#go_sources[@]}" -gt 0 ]; then
    gofmt -w "${go_sources[@]}"
  fi
}

check_format() {
  require_environment
  require_command clang-format
  mapfile -t cxx_sources < <(cpp_files)
  if [ "${#cxx_sources[@]}" -gt 0 ]; then
    clang-format --dry-run --Werror "${cxx_sources[@]}"
  fi
  "$venv_dir/bin/ruff" format --check python tools
  mapfile -t go_sources < <(go_files)
  gofmt_output=$(gofmt -l "${go_sources[@]}")
  if [ -n "$gofmt_output" ]; then
    printf 'Go files require formatting:\n%s\n' "$gofmt_output" >&2
    exit 1
  fi
}

lint_sources() {
  require_environment
  check_format
  "$venv_dir/bin/ruff" check python tools
  "$venv_dir/bin/mypy" python/intelligence python/model_serving python/tests tools
  (
    cd control
    go vet ./...
  )
  cmake --fresh --preset ci
  cmake --build --preset ci --parallel

  if command -v shellcheck >/dev/null 2>&1; then
    shellcheck tools/run.sh tools/toolchain.sh
  else
    printf 'NOTE: shellcheck unavailable; CI installs and enforces it.\n'
  fi
}

run_tests() {
  require_environment
  mkdir -p build/reports/cpp build/reports/python build/reports/control
  cmake --preset dev
  cmake --build --preset dev --parallel
  ctest --preset dev --output-junit build/reports/cpp/junit.xml
  "$venv_dir/bin/pytest"
  (
    cd control
    go test ./...
    go test -json ./... > ../build/reports/control/test.json
    go test -coverprofile=../build/reports/control/coverage.out ./...
  )
}

run_sanitizers() {
  require_environment
  mkdir -p build/reports/cpp build/reports/control

  cmake --preset asan
  cmake --build --preset asan --parallel
  ctest \
    --preset asan \
    --output-junit build/reports/cpp/asan-junit.xml

  cmake --preset ubsan
  cmake --build --preset ubsan --parallel
  ctest \
    --preset ubsan \
    --output-junit build/reports/cpp/ubsan-junit.xml

  cmake --preset tsan
  cmake --build --preset tsan --parallel
  ctest --preset tsan --output-junit build/reports/cpp/tsan-junit.xml

  (
    cd control
    go test -race ./...
  )
}

run_fuzz() {
  require_environment
  local artifact_dir="$repository_root/build/fuzz-artifacts/audit-envelope"
  local corpus_parent="$repository_root/build/fuzz-corpus"
  local corpus_dir
  cmake --preset fuzz
  cmake --build --preset fuzz \
    --target aegis_audit_envelope_fuzz aegis_smx_packet_fuzz \
      aegis_feed_decoder_fuzz aegis_oms_state_fuzz \
      aegis_gateway_decoder_fuzz aegis_journal_frame_fuzz --parallel
  mkdir -p "$artifact_dir" "$corpus_parent"
  corpus_dir=$(mktemp -d "$corpus_parent/audit-envelope.XXXXXX")
  trap 'rm -rf -- "$corpus_dir"' RETURN
  cp schemas/golden/data_quality_v1.amae "$corpus_dir/data_quality_v1.amae"
  cp schemas/golden/model_forecast_v1_3.amae \
    "$corpus_dir/model_forecast_v1_3.amae"
  build/fuzz/cpp/common/aegis_audit_envelope_fuzz \
    -runs="${AEGIS_FUZZ_RUNS:-10000}" \
    -seed="${AEGIS_TEST_SEED:-20260828}" \
    -max_len=4194304 \
    -artifact_prefix="$artifact_dir/" \
    "$corpus_dir"
  build/fuzz/cpp/market_data/aegis_smx_packet_fuzz \
    -runs="${AEGIS_FUZZ_RUNS:-10000}" \
    -seed="${AEGIS_TEST_SEED:-20260828}" \
    -max_len=156 \
    -artifact_prefix="$artifact_dir/"
  build/fuzz/cpp/market_data/aegis_feed_decoder_fuzz \
    -runs="${AEGIS_FUZZ_RUNS:-10000}" \
    -seed="${AEGIS_TEST_SEED:-20260828}" \
    -max_len=2048 \
    -artifact_prefix="$artifact_dir/"
  build/fuzz/cpp/oms/aegis_oms_state_fuzz \
    -runs="${AEGIS_FUZZ_RUNS:-10000}" \
    -seed="${AEGIS_TEST_SEED:-20260828}" \
    -max_len=256 \
    -artifact_prefix="$artifact_dir/"
  build/fuzz/cpp/execution/aegis_gateway_decoder_fuzz \
    -runs="${AEGIS_FUZZ_RUNS:-10000}" \
    -seed="${AEGIS_TEST_SEED:-20260828}" \
    -max_len=512 \
    -artifact_prefix="$artifact_dir/"
  build/fuzz/cpp/journal/aegis_journal_frame_fuzz \
    -runs="${AEGIS_FUZZ_RUNS:-10000}" \
    -seed="${AEGIS_TEST_SEED:-20260828}" \
    -max_len=4096 \
    -artifact_prefix="$artifact_dir/"
}

run_benchmark() {
  require_environment
  mkdir -p build/reports/benchmarks
  cmake --preset release
  cmake --build --preset release --parallel
  build/release/cpp/benchmarks/aegis_benchmark_smoke \
    --benchmark_min_time="${AEGIS_BENCHMARK_MIN_TIME:-0.01s}" \
    --benchmark_repetitions=1 \
    --benchmark_report_aggregates_only=true \
    --benchmark_out=build/reports/benchmarks/smoke.json \
    --benchmark_out_format=json
  "$python_bin" tools/benchmark_timeseries.py \
    --iterations="${AEGIS_TIMESERIES_BENCHMARK_ITERATIONS:-100}" \
    --output=build/reports/benchmarks/timeseries.json
  "$python_bin" tools/benchmark_news_intelligence.py \
    --iterations="${AEGIS_NEWS_BENCHMARK_ITERATIONS:-100}" \
    --output=build/reports/benchmarks/news-intelligence.json
  "$python_bin" tools/benchmark_earnings_specialist.py \
    --iterations="${AEGIS_EARNINGS_BENCHMARK_ITERATIONS:-100}" \
    --output=build/reports/benchmarks/earnings-specialist.json
  "$python_bin" tools/benchmark_macro_specialist.py \
    --iterations="${AEGIS_MACRO_BENCHMARK_ITERATIONS:-100}" \
    --output=build/reports/benchmarks/macro-specialist.json
  "$python_bin" tools/benchmark_options_analytics.py \
    --iterations="${AEGIS_OPTIONS_BENCHMARK_ITERATIONS:-100}" \
    --output=build/reports/benchmarks/options-analytics.json
  "$python_bin" tools/benchmark_market_specialists.py \
    --iterations="${AEGIS_MARKET_SPECIALISTS_BENCHMARK_ITERATIONS:-100}" \
    --output=build/reports/benchmarks/market-specialists.json
}

check_docs() {
  require_environment
  "$python_bin" tools/docs_check.py
}

check_schemas() {
  require_environment
  local flatc_bin
  flatc_bin=$(schema_flatc | tail -n 1)
  "$python_bin" tools/schema_codegen.py --flatc "$flatc_bin" --check
  "$python_bin" tools/generate_schema_golden.py --check
}

generate_schemas() {
  require_environment
  local flatc_bin
  flatc_bin=$(schema_flatc | tail -n 1)
  "$python_bin" tools/schema_codegen.py --flatc "$flatc_bin" --write
  "$python_bin" tools/generate_schema_golden.py
}

scan_dependencies() {
  require_environment
  "$venv_dir/bin/pip-audit" \
    --require-hashes \
    --requirement python/requirements-dev.lock
  (
    cd control
    go run golang.org/x/vuln/cmd/govulncheck@v1.7.0 ./...
  )

  mapfile -d '' -t scan_files < <(
    find . \
      -path './.git' -prune -o \
      -path './.venv' -prune -o \
      -path './.mypy_cache' -prune -o \
      -path './.pytest_cache' -prune -o \
      -path './.ruff_cache' -prune -o \
      -path './.coverage' -prune -o \
      -path './_CPack_Packages' -prune -o \
      -path './build' -prune -o \
      -path './dist' -prune -o \
      -path './.secrets.baseline' -prune -o \
      -name '__pycache__' -prune -o \
      -type f -printf '%P\0'
  )
  "$venv_dir/bin/detect-secrets-hook" \
    --baseline .secrets.baseline \
    "${scan_files[@]}"
}

package_artifacts() {
  require_environment
  mkdir -p dist/cpp dist/python dist/control
  cmake --preset release
  cmake --build --preset release --parallel
  cpack --config build/release/CPackConfig.cmake -B dist/cpp
  "$python_bin" -m build --no-isolation --outdir dist/python
  (
    cd control
    go test ./...
    go build \
      -buildmode=archive \
      -buildvcs=false \
      -trimpath \
      -o ../dist/control/config_service.a \
      ./config_service
  )
  "$python_bin" tools/generate_sbom.py --output dist/aegis-mx.cdx.json
}

run_fast() {
  "$0" docs-check
  "$0" schemas-check
  "$0" lint
  "$0" test
  "$0" benchmark
}

run_full() {
  "$0" fast
  "$0" test-sanitizers
  "$0" test-fuzz
  "$0" dependency-scan
  "$0" package
}

command_name=${1:-help}
case "$command_name" in
  help) print_help ;;
  bootstrap) bootstrap ;;
  format) format_sources ;;
  format-check) check_format ;;
  lint) lint_sources ;;
  test) run_tests ;;
  test-sanitizers) run_sanitizers ;;
  test-fuzz) run_fuzz ;;
  benchmark) run_benchmark ;;
  package) package_artifacts ;;
  docs-check) check_docs ;;
  schemas-check) check_schemas ;;
  schemas-generate) generate_schemas ;;
  dependency-scan) scan_dependencies ;;
  fast) run_fast ;;
  full) run_full ;;
  *)
    printf 'Unknown task: %s\n' "$command_name" >&2
    print_help >&2
    exit 2
    ;;
esac

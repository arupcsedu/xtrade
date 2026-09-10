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
    '  make benchmark-platform Run the qualified full-platform benchmark suite.' \
    '  make benchmark-platform-smoke Run the bounded full-platform smoke suite.' \
    '  make benchmark-regression Compare a platform report with an approved baseline.' \
    '  make paper-integration Run all deterministic full-system PAPER scenarios.' \
    '  make paper-soak      Run a configurable local PAPER soak worker.' \
    '  make paper-soak-smoke Run the bounded PAPER soak validation.' \
    '  make operator-simulation Run the PAPER-only hierarchical kill drill.' \
    '  make package          Build C++, Python, control, and SBOM artifacts.' \
    '  make docs-check       Validate local documentation.' \
    '  make schemas-check    Verify generated bindings and golden schema files.' \
    '  make schemas-generate Regenerate bindings and golden schema files.' \
    '  make dependency-scan  Audit Python and Go dependencies and visible secrets.' \
    '  make security-test    Validate security policy and adversarial boundaries.' \
    '  make chaos-fast       Run every bounded single-fault chaos scenario.' \
    '  make chaos-nightly    Run the deterministic chaos soak and fault combinations.' \
    '  make edge-validate    Validate non-live edge profiles and deployment contracts.' \
    '  make edge-package     Build and verify deterministic rollback packages.' \
    '  make regional-validate Validate non-hot-path Kubernetes deployment contracts.' \
    '  make reproducibility-check Build twice and compare packaged artifacts.' \
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
  "$venv_dir/bin/mypy" \
    python/intelligence python/model_serving python/research python/tests tools
  (
    cd control
    go vet ./...
  )
  cmake --fresh --preset ci
  cmake --build --preset ci --parallel

  if command -v shellcheck >/dev/null 2>&1; then
    shellcheck \
      tools/run.sh tools/toolchain.sh tools/slurm/paper-integration.sbatch \
      tools/slurm/paper-soak.sbatch
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
  cp schemas/golden/model_forecast_v1_9.amae \
    "$corpus_dir/model_forecast_v1_9.amae"
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
  "$python_bin" tools/benchmark_point_in_time.py \
    --iterations="${AEGIS_POINT_IN_TIME_BENCHMARK_ITERATIONS:-100}" \
    --records="${AEGIS_POINT_IN_TIME_BENCHMARK_RECORDS:-256}" \
    --output=build/reports/benchmarks/point-in-time.json
  "$python_bin" tools/benchmark_data_repository.py \
    --iterations="${AEGIS_DATA_REPOSITORY_BENCHMARK_ITERATIONS:-25}" \
    --file-count="${AEGIS_DATA_REPOSITORY_BENCHMARK_FILES:-64}" \
    --output=build/reports/benchmarks/data-repository.json
  (
    cd control
    go test -run '^$' \
      -bench 'Benchmark(VerifySignedManifest|FeatureSchemaCompatibility|InspectLineage|DeploymentObservation)$' \
      -benchmem -count=1 ./model_registry |
      tee ../build/reports/benchmarks/model-registry.txt
    go test -run '^$' \
      -bench '^BenchmarkEdgeEvaluate$' \
      -benchmem -count=1 ./config_service |
      tee ../build/reports/benchmarks/config-service.txt
  )
  run_platform_benchmark \
    "${AEGIS_PLATFORM_BENCHMARK_SMOKE_SAMPLES:-32}" \
    "${AEGIS_PLATFORM_BENCHMARK_SMOKE_WARMUP:-8}"
}

run_platform_benchmark() {
  require_environment
  local samples=${1:-${AEGIS_PLATFORM_BENCHMARK_SAMPLES:-10000}}
  local warmup=${2:-${AEGIS_PLATFORM_BENCHMARK_WARMUP:-4096}}
  mkdir -p build/reports/benchmarks
  cmake --preset release
  cmake --build --preset release --target aegis_platform_benchmark --parallel
  build/release/cpp/benchmarks/aegis_platform_benchmark \
    --metadata build/reports/benchmarks/platform-sampler.json \
    --samples-output build/reports/benchmarks/platform-samples.ndjson \
    --samples "$samples" \
    --warmup "$warmup" \
    --seed "${AEGIS_PLATFORM_BENCHMARK_SEED:-20260906}"
  "$python_bin" tools/performance_benchmark.py summarize \
    --metadata build/reports/benchmarks/platform-sampler.json \
    --samples build/reports/benchmarks/platform-samples.ndjson \
    --output build/reports/benchmarks/platform-report.json
}

run_benchmark_regression() {
  require_environment
  local baseline=${AEGIS_BENCHMARK_BASELINE:-}
  local candidate=${AEGIS_BENCHMARK_CANDIDATE:-build/reports/benchmarks/platform-report.json}
  if [ -z "$baseline" ]; then
    printf '%s\n' 'AEGIS_BENCHMARK_BASELINE must name an approved report.' >&2
    return 2
  fi
  if [ ! -f "$candidate" ]; then
    run_platform_benchmark
  fi
  "$python_bin" tools/performance_benchmark.py compare \
    --baseline "$baseline" \
    --candidate "$candidate" \
    --output build/reports/benchmarks/regression.json
}

run_paper_integration() {
  require_environment
  mkdir -p build/reports/paper-trading
  cmake --preset release
  cmake --build --preset release --target aegis_paper_acceptance --parallel
  build/release/cpp/integration/aegis-paper-acceptance \
    --seed "${AEGIS_TEST_SEED:-20260906}" \
    --machine build/reports/paper-trading/acceptance-report.json \
    --human build/reports/paper-trading/system-report.md
  "$python_bin" -m json.tool \
    build/reports/paper-trading/acceptance-report.json >/dev/null
}

run_paper_soak() {
  require_environment
  local output_dir=${AEGIS_SOAK_OUTPUT_DIR:-build/reports/paper-soak/local}
  local event_count=${AEGIS_SOAK_EVENTS:-1000000}
  local cycle_count=${AEGIS_SOAK_CYCLES:-2}
  mkdir -p "$output_dir"
  cmake --preset release
  cmake --build --preset release --parallel
  "$python_bin" tools/paper_soak.py worker \
    --build-dir build/release \
    --output-dir "$output_dir" \
    --summary "$output_dir/worker-0.json" \
    --worker-id 0 \
    --seed "${AEGIS_TEST_SEED:-20260907}" \
    --events "$event_count" \
    --session-events "${AEGIS_SOAK_SESSION_EVENTS:-100000}" \
    --sample-limit "${AEGIS_SOAK_SAMPLE_LIMIT:-8192}" \
    --realtime-seconds "${AEGIS_SOAK_REALTIME_SECONDS:-1}" \
    --realtime-rate "${AEGIS_SOAK_REALTIME_RATE:-1000}" \
    --cycles "$cycle_count"
  "$python_bin" tools/paper_soak.py aggregate \
    --worker "$output_dir/worker-0.json" \
    --minimum-primary-events "$event_count" \
    --minimum-acceptance-cycles "$cycle_count" \
    --output "$output_dir/paper-soak-report.json" \
    --human "$output_dir/stability-report.md"
}

run_paper_soak_smoke() {
  AEGIS_SOAK_EVENTS=${AEGIS_SOAK_SMOKE_EVENTS:-100000} \
    AEGIS_SOAK_SESSION_EVENTS=${AEGIS_SOAK_SMOKE_SESSION_EVENTS:-20000} \
    AEGIS_SOAK_CYCLES=${AEGIS_SOAK_SMOKE_CYCLES:-1} \
    AEGIS_SOAK_REALTIME_SECONDS=0 \
    AEGIS_SOAK_OUTPUT_DIR=build/reports/paper-soak/smoke \
    run_paper_soak
}

run_operator_simulation() {
  require_environment
  local output_dir=${AEGIS_OPERATOR_OUTPUT_DIR:-build/reports/operations}
  mkdir -p "$output_dir"
  cmake --preset release
  cmake --build --preset release --target aegis_operator_simulation --parallel
  build/release/cpp/integration/aegis-operator-simulation \
    --seed "${AEGIS_TEST_SEED:-20260908}" \
    --machine "$output_dir/operator-simulation.json" \
    --audit "$output_dir/operator-audit.ndjson"
  "$python_bin" -m json.tool "$output_dir/operator-simulation.json" >/dev/null
  "$python_bin" tools/operational_readiness.py \
    --repository "$repository_root" \
    --operator-report "$output_dir/operator-simulation.json" \
    --build-dir build/release \
    --output "$output_dir/live-mode-disabled.json" \
    --human "$output_dir/live-mode-disabled.md"
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

run_security_tests() {
  require_environment
  mkdir -p build/reports/security
  "$python_bin" tools/generate_sbom.py \
    --output build/reports/security/aegis-mx.cdx.json
  "$python_bin" tools/security_policy.py \
    --sbom build/reports/security/aegis-mx.cdx.json
  "$python_bin" tools/regional_deployment.py validate-assets |
    tee build/reports/security/regional-deployment.json
  "$python_bin" -m pytest -q python/tests/test_security_hardening.py --no-cov
}

run_chaos_fast() {
  require_environment
  mkdir -p build/reports/chaos
  "$python_bin" tools/chaos_runner.py \
    --profile fast \
    --iterations "${AEGIS_CHAOS_FAST_ITERATIONS:-1}" \
    --seed "${AEGIS_TEST_SEED:-20260828}" \
    --output build/reports/chaos/fast.json
}

run_chaos_nightly() {
  require_environment
  mkdir -p build/reports/chaos
  "$python_bin" tools/chaos_runner.py \
    --profile nightly \
    --iterations "${AEGIS_CHAOS_NIGHTLY_ITERATIONS:-1000}" \
    --seed "${AEGIS_TEST_SEED:-20260828}" \
    --output build/reports/chaos/nightly.json
}

validate_edge_deployment() {
  require_environment
  mkdir -p build/reports/edge-deployment
  "$python_bin" tools/edge_deployment.py validate-assets |
    tee build/reports/edge-deployment/assets.json
  local profile
  for profile in development ci replay paper staging production-disabled; do
    "$python_bin" tools/edge_deployment.py validate-profile \
      --profile "$profile" |
      tee "build/reports/edge-deployment/profile-${profile}.json"
  done
}

validate_regional_deployment() {
  require_environment
  mkdir -p build/reports/regional-deployment
  "$python_bin" tools/regional_deployment.py validate-assets |
    tee build/reports/regional-deployment/assets.json
}

package_edge_rollback() {
  require_environment
  validate_edge_deployment
  mkdir -p dist/edge build/reports/edge-deployment
  local profile
  local artifact
  for profile in development ci replay paper staging production-disabled; do
    artifact="dist/edge/aegis-edge-rollback-${profile}.tar.gz"
    "$python_bin" tools/edge_deployment.py package-rollback \
      --profile "$profile" \
      --source-date-epoch "${SOURCE_DATE_EPOCH}" \
      --output "$artifact"
    "$python_bin" tools/edge_deployment.py verify-rollback \
      --package "$artifact" |
      tee "build/reports/edge-deployment/rollback-${profile}.json"
  done
}

package_artifacts() {
  require_environment
  require_command gzip
  require_command tar
  mkdir -p dist/cpp dist/python dist/control
  cmake --preset release
  cmake --build --preset release --parallel
  local cpack_root="$repository_root/build/package/cpack"
  rm -rf -- "$cpack_root" "$repository_root/dist/cpp/_CPack_Packages"
  mkdir -p "$cpack_root"
  cpack --config build/release/CPackConfig.cmake -B "$cpack_root"
  mapfile -t cpack_archives < <(
    find "$cpack_root" -maxdepth 1 -type f -name '*.tar.gz' -print
  )
  mapfile -t cpack_stages < <(
    find "$cpack_root/_CPack_Packages/Linux/TGZ" \
      -mindepth 1 -maxdepth 1 -type d -print
  )
  if [ "${#cpack_archives[@]}" -ne 1 ] || [ "${#cpack_stages[@]}" -ne 1 ]; then
    printf '%s\n' 'CPack did not produce exactly one archive and staging tree.' >&2
    return 1
  fi
  local cpp_archive="$repository_root/dist/cpp/$(basename "${cpack_archives[0]}")"
  local cpp_archive_temp="${cpp_archive}.tmp"
  local cpack_stage_parent
  local cpack_stage_name
  cpack_stage_parent=$(dirname "${cpack_stages[0]}")
  cpack_stage_name=$(basename "${cpack_stages[0]}")
  tar \
    --sort=name \
    --mtime="@${SOURCE_DATE_EPOCH}" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --format=gnu \
    -C "$cpack_stage_parent" \
    -cf - \
    "$cpack_stage_name" |
    gzip -n >"$cpp_archive_temp"
  mv -f -- "$cpp_archive_temp" "$cpp_archive"
  "$python_bin" -m build --no-isolation --outdir dist/python
  (
    cd control
    go test ./...
    CGO_ENABLED=0 go build \
      -buildmode=archive \
      -buildvcs=false \
      -ldflags=-buildid= \
      -trimpath \
      -o ../dist/control/config_service.a \
      ./config_service
    CGO_ENABLED=0 go build \
      -buildvcs=false \
      -ldflags=-buildid= \
      -trimpath \
      -o ../dist/control/config-service \
      ./cmd/config-service
    CGO_ENABLED=0 go build \
      -buildvcs=false \
      -ldflags=-buildid= \
      -trimpath \
      -o ../dist/control/model-registry \
      ./cmd/model-registry
    CGO_ENABLED=0 go build \
      -buildvcs=false \
      -ldflags=-buildid= \
      -trimpath \
      -o ../dist/control/aegis-edge-config-verify \
      ./cmd/edge-config-verify
  )
  "$python_bin" tools/generate_sbom.py --output dist/aegis-mx.cdx.json
  package_edge_rollback
}

check_reproducibility() {
  require_environment
  local comparison_root
  comparison_root=$(mktemp -d "$repository_root/build/reproducibility.XXXXXX")
  trap 'rm -rf -- "$comparison_root"' RETURN
  package_artifacts
  mkdir -p "$comparison_root/first"
  cp -a dist/. "$comparison_root/first/"
  package_artifacts
  "$python_bin" tools/verify_reproducible_artifacts.py \
    "$comparison_root/first" dist
}

run_fast() {
  "$0" docs-check
  "$0" schemas-check
  "$0" security-test
  "$0" chaos-fast
  "$0" edge-validate
  "$0" regional-validate
  "$0" lint
  "$0" test
  "$0" paper-integration
  "$0" operator-simulation
  "$0" benchmark
}

run_full() {
  "$0" fast
  "$0" chaos-nightly
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
  benchmark-platform) run_platform_benchmark ;;
  benchmark-platform-smoke)
    run_platform_benchmark \
      "${AEGIS_PLATFORM_BENCHMARK_SMOKE_SAMPLES:-32}" \
      "${AEGIS_PLATFORM_BENCHMARK_SMOKE_WARMUP:-8}"
    ;;
  benchmark-regression) run_benchmark_regression ;;
  paper-integration) run_paper_integration ;;
  paper-soak) run_paper_soak ;;
  paper-soak-smoke) run_paper_soak_smoke ;;
  operator-simulation) run_operator_simulation ;;
  package) package_artifacts ;;
  docs-check) check_docs ;;
  schemas-check) check_schemas ;;
  schemas-generate) generate_schemas ;;
  dependency-scan) scan_dependencies ;;
  security-test) run_security_tests ;;
  chaos-fast) run_chaos_fast ;;
  chaos-nightly) run_chaos_nightly ;;
  edge-validate) validate_edge_deployment ;;
  edge-package) package_edge_rollback ;;
  regional-validate) validate_regional_deployment ;;
  reproducibility-check) check_reproducibility ;;
  fast) run_fast ;;
  full) run_full ;;
  *)
    printf 'Unknown task: %s\n' "$command_name" >&2
    print_help >&2
    exit 2
    ;;
esac

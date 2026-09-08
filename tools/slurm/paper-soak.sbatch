#!/usr/bin/env bash
#SBATCH --job-name=aegis-paper-soak
#SBATCH --partition=parallel
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --time=00:45:00

set -euo pipefail

repository_root=${SLURM_SUBMIT_DIR:?submit the job from the repository root}
cd "$repository_root"
if [ ! -f CMakeLists.txt ] || [ ! -f docs/architecture/engineering-contract.md ]; then
  printf 'SLURM_SUBMIT_DIR is not the Aegis-MX repository root: %s\n' \
    "$repository_root" >&2
  exit 2
fi

# shellcheck source=tools/toolchain.sh
source "$repository_root/tools/toolchain.sh"

export AEGIS_PYTHON_ENV="${AEGIS_PYTHON_ENV:-/scratch/djy8hg/env/aegis_mx_contracts}"
base_seed=${AEGIS_TEST_SEED:-20260907}
events_per_worker=${AEGIS_SOAK_EVENTS_PER_WORKER:-1000000000}
session_events=${AEGIS_SOAK_SESSION_EVENTS:-10000000}
cycle_count=${AEGIS_SOAK_CYCLES:-10}
realtime_seconds=${AEGIS_SOAK_REALTIME_SECONDS:-60}
realtime_rate=${AEGIS_SOAK_REALTIME_RATE:-1000}
sample_limit=${AEGIS_SOAK_SAMPLE_LIMIT:-8192}
output_dir=${AEGIS_SOAK_OUTPUT_DIR:-build/reports/paper-soak/slurm-${SLURM_JOB_ID}}
python_bin="$AEGIS_PYTHON_ENV/bin/python"

mkdir -p "$output_dir"
cmake --preset release
cmake --build --preset release --parallel

export AEGIS_SOAK_BASE_SEED="$base_seed"
export AEGIS_SOAK_EVENTS_PER_WORKER="$events_per_worker"
export AEGIS_SOAK_SESSION_EVENTS="$session_events"
export AEGIS_SOAK_CYCLES="$cycle_count"
export AEGIS_SOAK_REALTIME_SECONDS="$realtime_seconds"
export AEGIS_SOAK_REALTIME_RATE="$realtime_rate"
export AEGIS_SOAK_SAMPLE_LIMIT="$sample_limit"
export AEGIS_SOAK_OUTPUT_DIR="$output_dir"
export AEGIS_SOAK_BUILD_DIR="$repository_root/build/release"
export AEGIS_SOAK_PYTHON_BIN="$python_bin"

srun --kill-on-bad-exit=1 bash -c '
  worker_id=$SLURM_PROCID
  worker_seed=$((AEGIS_SOAK_BASE_SEED + worker_id * 1000003))
  exec "$AEGIS_SOAK_PYTHON_BIN" tools/paper_soak.py worker \
    --build-dir "$AEGIS_SOAK_BUILD_DIR" \
    --output-dir "$AEGIS_SOAK_OUTPUT_DIR" \
    --summary "$AEGIS_SOAK_OUTPUT_DIR/worker-${worker_id}.json" \
    --worker-id "$worker_id" \
    --seed "$worker_seed" \
    --events "$AEGIS_SOAK_EVENTS_PER_WORKER" \
    --session-events "$AEGIS_SOAK_SESSION_EVENTS" \
    --sample-limit "$AEGIS_SOAK_SAMPLE_LIMIT" \
    --realtime-seconds "$AEGIS_SOAK_REALTIME_SECONDS" \
    --realtime-rate "$AEGIS_SOAK_REALTIME_RATE" \
    --cycles "$AEGIS_SOAK_CYCLES"
'

worker_arguments=()
for ((worker_id = 0; worker_id < SLURM_NTASKS; ++worker_id)); do
  worker_arguments+=(--worker "$output_dir/worker-${worker_id}.json")
done
minimum_events=$((events_per_worker * SLURM_NTASKS))
minimum_cycles=$((cycle_count * SLURM_NTASKS))
"$python_bin" tools/paper_soak.py aggregate \
  "${worker_arguments[@]}" \
  --minimum-primary-events "$minimum_events" \
  --minimum-acceptance-cycles "$minimum_cycles" \
  --output "$output_dir/paper-soak-report.json" \
  --human "$output_dir/stability-report.md"

printf 'PAPER soak evidence: %s\n' "$output_dir"

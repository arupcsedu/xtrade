#!/usr/bin/env bash
#SBATCH --job-name=aegis-alpaca-paper
#SBATCH --partition=parallel
# The site parallel partition enforces a two-node minimum. The batch process
# runs exactly once; no srun is used, preventing duplicate order submissions.
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
# The parallel QOS requires at least four CPUs across at least two nodes.
#SBATCH --cpus-per-task=2
#SBATCH --time=00:10:00

set -euo pipefail

repository_root=${SLURM_SUBMIT_DIR:?submit the job from the repository root}
cd "$repository_root"
if [ ! -f CMakeLists.txt ] || [ ! -f ticker.txt ]; then
  printf 'SLURM_SUBMIT_DIR is not the Aegis-MX repository root\n' >&2
  exit 2
fi

python_environment=${AEGIS_PYTHON_ENV:-/scratch/djy8hg/env/aegis_mx_contracts}
authorization=${AEGIS_ALPACA_AUTHORIZATION:?set the private authorization path}
report_root=${AEGIS_ALPACA_REPORT_ROOT:?set the external report directory}
symbol=${AEGIS_ALPACA_SYMBOL:-NVDA}
path_seed=${AEGIS_ALPACA_PATH_SEED:-20260911}
path_binary=$repository_root/build/release/cpp/integration/aegis-alpaca-paper-path

umask 077
mkdir -p "$report_root"
# The target is rebuilt from the checked-out source in an explicitly non-live
# configuration before credentials are loaded or any provider call is made.
# shellcheck source=tools/toolchain.sh
source "$repository_root/tools/toolchain.sh"
cmake --fresh --preset release -DAEGIS_ENABLE_LIVE_TRADING=OFF
cmake --build --preset release --target aegis_alpaca_paper_path --parallel
"$path_binary" --symbol "$symbol" --seed "$path_seed" \
  > "$report_root/system-path-local.json"
"$python_environment/bin/python" tools/alpaca_paper_certification.py \
  preflight --symbol "$symbol" --report "$report_root/preflight.json"
"$python_environment/bin/python" tools/alpaca_paper_certification.py \
  certify-system-path --symbol "$symbol" \
  --authorization "$authorization" \
  --report "$report_root/certification.json" \
  --path-binary "$path_binary" \
  --seed "$path_seed" \
  --execute-paper

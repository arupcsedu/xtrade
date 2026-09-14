#!/usr/bin/env bash
#SBATCH --job-name=aegis-real-poc
#SBATCH --partition=parallel
# The site parallel partition requires two nodes. One batch process owns the
# SQLite spool and publications; no second writer is launched.
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --time=12:00:00

set -euo pipefail

repository_root=${SLURM_SUBMIT_DIR:?submit from the Aegis-MX repository root}
python_environment=${AEGIS_PYTHON_ENV:-/scratch/djy8hg/env/aegis_mx_contracts}
data_root=${AEGIS_POC_DATA_ROOT:-/scratch/djy8hg/aegis_mx_poc_data}

cd "$repository_root"
if [ ! -f CMakeLists.txt ] || [ ! -f ticker.txt ]; then
  printf 'SLURM_SUBMIT_DIR is not the Aegis-MX repository root\n' >&2
  exit 2
fi

umask 077
export PYTHONPATH="$repository_root/python/research:$repository_root/python/intelligence:$repository_root"

"$python_environment/bin/python" -m aegis_mx_research.real_minute_pipeline \
  --data-root "$data_root" promote --execute
"$python_environment/bin/python" -m aegis_mx_research.real_minute_pipeline \
  --data-root "$data_root" verify
"$python_environment/bin/python" tools/build_real_feature_dataset.py \
  --data-root "$data_root" --execute

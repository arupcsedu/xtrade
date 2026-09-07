#!/usr/bin/env bash
#SBATCH --job-name=aegis-paper-e2e
#SBATCH --partition=parallel
# The site parallel partition enforces a two-node minimum. The batch process
# runs once; the second task is reserved for scheduler-valid placement only.
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --time=00:20:00

set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repository_root"

export AEGIS_PYTHON_ENV="${AEGIS_PYTHON_ENV:-/scratch/djy8hg/env/aegis_mx_contracts}"
export AEGIS_TEST_SEED="${AEGIS_TEST_SEED:-20260906}"

make paper-integration

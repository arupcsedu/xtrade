#!/usr/bin/env bash
#SBATCH --job-name=aegis-iex-backfill
#SBATCH --partition=parallel
# The site parallel partition requires two nodes. The batch shell performs the
# download exactly once on its assigned node; this script intentionally uses no
# srun and launches no duplicate workers.
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --time=02:00:00

set -euo pipefail

repository_root=${SLURM_SUBMIT_DIR:?submit from the Aegis-MX repository root}
python_environment=${AEGIS_PYTHON_ENV:-/scratch/djy8hg/env/aegis_mx_contracts}
data_root=${AEGIS_POC_DATA_ROOT:-/scratch/djy8hg/aegis_mx_poc_data}
approval=${AEGIS_ALPACA_DATA_APPROVAL:-$data_root/manifests/approvals/alpaca-iex-academic-approval-v1.json}
policy=${AEGIS_ALPACA_DATA_POLICY:-$data_root/manifests/approvals/alpaca-iex-academic-policy-v1.json}
pilot_acceptance=${AEGIS_ALPACA_PILOT_ACCEPTANCE:-$data_root/reports/alpaca-iex-minute/pilot/acceptance.json}
secrets_file=${AEGIS_ALPACA_SECRETS_FILE:-/scratch/djy8hg/ALPACA/.keys}

cd "$repository_root"
if [ ! -f CMakeLists.txt ] || [ ! -f ticker.txt ]; then
  printf 'SLURM_SUBMIT_DIR is not the Aegis-MX repository root\n' >&2
  exit 2
fi

umask 077
export PYTHONPATH="$repository_root/python/research:$repository_root"
"$python_environment/bin/python" -m aegis_mx_research.alpaca_historical \
  --data-root "$data_root" backfill \
  --policy "$policy" \
  --approval "$approval" \
  --pilot-acceptance "$pilot_acceptance" \
  --secrets-file "$secrets_file" \
  --execute

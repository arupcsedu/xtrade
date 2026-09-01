#!/usr/bin/env bash

# This file is sourced by repository commands. It intentionally changes only the
# current process environment and never installs system packages.

aegis_version_at_least() {
  local actual=$1
  local required=$2
  [ "$(printf '%s\n%s\n' "$required" "$actual" | sort -V | head -n 1)" = "$required" ]
}

aegis_command_version() {
  local command_name=$1
  local version_pattern=$2
  "$command_name" --version 2>/dev/null |
    sed -n "s/.*$version_pattern\([0-9][0-9.]*\).*/\1/p" |
    head -n 1
}

aegis_should_load_modules=0
if type module >/dev/null 2>&1 && [ "${AEGIS_SKIP_MODULES:-0}" != 1 ]; then
  cmake_version=$(aegis_command_version cmake 'version ' || true)
  gcc_version=$(aegis_command_version gcc ' ' || true)
  if [ -z "$cmake_version" ] || ! aegis_version_at_least "$cmake_version" 3.28; then
    aegis_should_load_modules=1
  elif [ -z "$gcc_version" ] || ! aegis_version_at_least "$gcc_version" 11; then
    aegis_should_load_modules=1
  elif ! command -v ninja >/dev/null 2>&1 || ! command -v go >/dev/null 2>&1; then
    aegis_should_load_modules=1
  elif ! command -v clang-format >/dev/null 2>&1 || ! command -v clang-tidy >/dev/null 2>&1; then
    aegis_should_load_modules=1
  fi
fi

if [ "$aegis_should_load_modules" = 1 ]; then
  module load gcc/14.2.0
  module load llvm/21.1.8
  module load cmake/3.28.1
  module load ninja/1.13.1
  module load go/1.26.4
fi

export CC="${CC:-gcc}"
export CXX="${CXX:-g++}"
export PYTHONHASHSEED="${PYTHONHASHSEED:-20260828}"
export AEGIS_TEST_SEED="${AEGIS_TEST_SEED:-20260828}"
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_NO_INPUT=1
# Do not inherit per-user package indexes or extra indexes. This keeps bootstrap
# deterministic and avoids dependency-confusion surprises. Controlled mirrors
# can be supplied explicitly by the operator through the AEGIS_* variables.
export PIP_CONFIG_FILE="${AEGIS_PIP_CONFIG_FILE:-/dev/null}"
export PIP_INDEX_URL="${AEGIS_PYPI_INDEX_URL:-https://pypi.org/simple}"
unset PIP_EXTRA_INDEX_URL
export GOFLAGS="${GOFLAGS:--mod=readonly}"

if git rev-parse --verify HEAD >/dev/null 2>&1; then
  export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD)}"
else
  export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
fi

unset aegis_should_load_modules cmake_version gcc_version

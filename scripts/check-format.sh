#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

source_files=()
while IFS= read -r file; do
  source_files+=("$file")
done < <(find src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
if [[ ${#source_files[@]} -eq 0 ]]; then
  echo "No C++ source files found" >&2
  exit 1
fi

clang-format --dry-run --Werror "${source_files[@]}"

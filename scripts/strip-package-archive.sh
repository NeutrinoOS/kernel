#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 PACKAGE.zip STRIP READELF" >&2
    exit 2
fi

archive=$(realpath "$1")
strip_tool=$2
readelf_tool=$3

if [[ ! -f "$archive" ]]; then
    echo "release package archive not found: $archive" >&2
    exit 1
fi

work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT
unzip -q "$archive" -d "$work_dir/root"

elf_count=0
bytes_before=0
bytes_after=0
while IFS= read -r -d '' file; do
    if "$readelf_tool" -h "$file" >/dev/null 2>&1; then
        size_before=$(stat -c %s "$file")
        "$strip_tool" --strip-unneeded "$file"
        size_after=$(stat -c %s "$file")
        elf_count=$((elf_count + 1))
        bytes_before=$((bytes_before + size_before))
        bytes_after=$((bytes_after + size_after))
    fi
done < <(find "$work_dir/root" -type f -print0)

if ((elf_count == 0)); then
    echo "[STRIP] $(basename "$archive"): no ELF payloads"
    exit 0
fi

replacement="$work_dir/$(basename "$archive")"
(
    cd "$work_dir/root"
    zip -0 -q -r "$replacement" .
)
unzip -tqq "$replacement"
mv -- "$replacement" "$archive"

echo "[STRIP] $(basename "$archive"): $elf_count ELF files, $bytes_before -> $bytes_after bytes"

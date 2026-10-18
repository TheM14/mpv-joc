#!/bin/bash
# Copy the MinGW runtime DLL closure of mpv.exe next to it, so the build runs
# without the MSYS2 environment on PATH.
#
# Usage: bundle_dlls.sh /path/to/mpv/build
set -e
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "${1:?usage: bundle_dlls.sh <mpv build dir>}"

collect() {
    ldd "$1" 2>/dev/null | awk '{print $3}' | grep '^/mingw64/' || true
}

declare -A seen
queue=("mpv.exe")
while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"
    queue=("${queue[@]:1}")
    for dll in $(collect "$cur"); do
        base=$(basename "$dll")
        if [ -n "${seen[$base]}" ]; then continue; fi
        seen[$base]=1
        queue+=("$dll")
    done
done

echo "DLLs to copy: ${#seen[@]}"
for name in "${!seen[@]}"; do
    cp -f "/mingw64/bin/$name" .
done
ls -1 *.dll | wc -l

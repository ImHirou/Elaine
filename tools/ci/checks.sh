#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The checks that need no build: licence headers, formatting, the generated tables against their
# lock, the blocks roadmap. CI runs this; run it yourself before a pull request.
set -uo pipefail
cd "$(dirname "$0")/../.."
fail=0

# Every source file we write says what its licence is. (JSON has no comments; Markdown is prose.)
while IFS= read -r f; do
    head -n 3 "$f" | grep -q 'SPDX-License-Identifier: GPL-3.0-or-later' || {
        echo "no SPDX-License-Identifier in its first lines: $f"
        fail=1
    }
done < <(git ls-files -- '*.cpp' '*.hpp' '*.py' '*.java' '*.sh' '*.cmake' 'CMakeLists.txt')

# Formatting: everything but src/proto/gen, where current.hpp is laid out by hand and the rest is generated.
if command -v clang-format >/dev/null; then
    git ls-files -z -- '*.cpp' '*.hpp' ':!src/proto/gen' | xargs -0 clang-format --dry-run --Werror || {
        echo "formatting: run clang-format -i on the files above ($(clang-format --version))"
        fail=1
    }
else
    echo "clang-format is not installed: formatting not checked"
    fail=1
fi

python3 tools/mcgen/mcgen.py check || fail=1

# The roadmap is generated from the jar's block classes: it needs the cache `mcgen.py setup` leaves.
python3 tools/mcgen/roadmap.py --check || {
    echo "docs/blocks-roadmap.md is stale: python3 tools/mcgen/roadmap.py"
    fail=1
}

[ $fail -eq 0 ] && echo "checks: all good"
exit $fail

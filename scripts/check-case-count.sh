#!/bin/bash
# Assert the suite ran the number of cases this repository declares.
#
# ctest counts a gtest binary as ONE entry, so a green ctest says nothing about
# how many cases actually ran: a case that stops being compiled disappears
# without a trace, and the gate stays green. The number therefore lives beside
# the tests in tests/expected-cases, and this script compares it against what
# gtest itself reported in the log the run left behind.
#
# Raise the number in the same commit that adds cases. Lowering it is a
# statement that cases were removed on purpose, and belongs in that commit's
# message.
#
# Usage: scripts/check-case-count.sh [<build-dir>]
# Exit codes: 0 match, 1 mismatch or no run found, 2 usage or missing declaration.
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
declared_file="${project_root}/tests/expected-cases"

if [ "$#" -gt 1 ]; then
    echo "usage: check-case-count.sh [<build-dir>]" >&2
    exit 2
fi

if [ ! -r "${declared_file}" ]; then
    echo "ERROR: ${declared_file} is missing -- the expected case count has no home." >&2
    exit 2
fi

declared="$(grep -Ev '^[[:space:]]*(#|$)' "${declared_file}" | head -1 | tr -d '[:space:]')"
case "${declared}" in
    ''|*[!0-9]*)
        echo "ERROR: ${declared_file} must start with a plain number." >&2
        exit 2
        ;;
esac

if [ "$#" -eq 1 ]; then
    log="$1/Testing/Temporary/LastTest.log"
else
    # Newest run wins: the plain and the sanitized build write to separate
    # directories and either may be the one just finished.
    log="$(ls -t "${project_root}"/build*/Testing/Temporary/LastTest.log 2>/dev/null | head -1 || true)"
fi

if [ -z "${log:-}" ] || [ ! -r "${log}" ]; then
    echo "ERROR: no ctest log found -- run the suite before checking its count." >&2
    exit 1
fi

# One summary line per test binary, so a repository with several of them needs
# the sum rather than the first match.
ran="$(sed -n 's/^\[==========\] \([0-9]\{1,\}\) tests\{0,1\} from .* ran.*/\1/p' "${log}" \
    | awk '{ s += $1 } END { print s + 0 }')"

if [ "${ran}" -eq 0 ]; then
    echo "ERROR: ${log} carries no gtest summary -- the suite did not run." >&2
    exit 1
fi

if [ "${ran}" != "${declared}" ]; then
    echo "ERROR: the suite ran ${ran} cases, tests/expected-cases declares ${declared}." >&2
    echo "       Raise the declaration in the commit that adds cases; lower it only" >&2
    echo "       when cases were removed on purpose." >&2
    exit 1
fi

echo "case count: ${ran} as declared (${log})"

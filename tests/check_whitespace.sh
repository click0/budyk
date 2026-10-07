#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# The whitespace rules from .editorconfig, over every tracked text file:
# no trailing blanks, no tabs (Makefiles excepted: make needs them, and
# the FreeBSD port's Makefile follows the ports tree's style), no CR,
# and a final newline. Prints each offending line or file; exits 1 if
# there is any. Run from anywhere inside the work tree:
#
#   tests/check_whitespace.sh

cd "$(git rev-parse --show-toplevel)" || exit 1
fail=0

if git grep -nIP '[ \t]+$'; then
    echo "^ trailing whitespace"; fail=1
fi
if git grep -nIP '\t' -- . ':(exclude,glob)**/Makefile' ':(exclude)Makefile'; then
    echo "^ tab characters (indent with spaces)"; fail=1
fi
if git grep -nIP '\r'; then
    echo "^ carriage returns (use LF line endings)"; fail=1
fi
missing=$(git grep -Il '' | while IFS= read -r f; do
    [ "$(tail -c1 "$f" | wc -l)" -eq 1 ] || echo "$f"
done)
if [ -n "$missing" ]; then
    echo "$missing"
    echo "^ no newline at the end of the file"; fail=1
fi

[ "$fail" -eq 0 ] && echo "whitespace: OK"
exit "$fail"

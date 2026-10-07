#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# apt-get update + install for the Ubuntu CI jobs, with a time limit on
# the network part:
#
#   .github/scripts/apt-install.sh <package>...
#
# An Ubuntu mirror that stops answering has held `apt-get update` until
# the job's timeout-minutes, with Acquire::http::Timeout set or not (the
# stall is not always on a socket read apt is timing). So the update
# and the package downloads run under timeout(1), three attempts each:
# both are safe to stop and repeat, and a repeated download keeps the
# .debs already fetched. Unpacking is not timed: stopping dpkg half-way
# leaves it "interrupted" and every later apt call fails.
set -u

attempt() {   # attempt <seconds> <command>...
    limit=$1; shift
    for i in 1 2 3; do
        timeout -k 10 "$limit" "$@" && return 0
        echo "apt-install: '$*' failed or ran past ${limit}s (attempt $i of 3)" >&2
        [ "$i" -lt 3 ] && sleep 5
    done
    return 1
}

echo 'Acquire::Retries "3"; Acquire::http::Timeout "30";' \
    | sudo tee /etc/apt/apt.conf.d/80-ci-timeouts > /dev/null
attempt 120 sudo apt-get update -qq || exit 1
attempt 240 sudo apt-get install -y --no-install-recommends --download-only "$@" || exit 1
sudo apt-get install -y --no-install-recommends "$@"

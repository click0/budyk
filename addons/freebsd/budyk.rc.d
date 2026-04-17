#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# PROVIDE: budyk
# REQUIRE: DAEMON NETWORKING
# KEYWORD: shutdown

. /etc/rc.subr

name="budyk"
rcvar="budyk_enable"
command="/usr/local/bin/budyk"
command_args="serve --config /usr/local/etc/budyk/config.yaml"
pidfile="/var/run/${name}.pid"

load_rc_config $name
: ${budyk_enable:="NO"}

run_rc_command "$1"

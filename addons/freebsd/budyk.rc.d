#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# PROVIDE: budyk
# REQUIRE: DAEMON NETWORKING
# KEYWORD: shutdown
#
# Add these lines to /etc/rc.conf.local or /etc/rc.conf
# to enable this service:
#
# budyk_enable (bool):         Set to NO by default.
#                              Set it to YES to enable budyk.
# budyk_user   (string):       Set user to run budyk as.
#                              Default: "budyk".
# budyk_group  (string):       Set group to run budyk as.
#                              Default: "budyk".
# budyk_config (string):       Path to config file.
#                              Default: "%%PREFIX%%/etc/budyk/config.yaml".
# budyk_datadir (string):      Ring-buffer data directory.
#                              Default: "/var/db/budyk".
# budyk_flags  (string):       Extra CLI flags.

. /etc/rc.subr

name="budyk"
rcvar="budyk_enable"

load_rc_config $name

: ${budyk_enable:="NO"}
: ${budyk_user:="budyk"}
: ${budyk_group:="budyk"}
: ${budyk_config:="%%PREFIX%%/etc/budyk/config.yaml"}
: ${budyk_datadir:="/var/db/budyk"}
: ${budyk_flags:=""}

pidfile="/var/run/${name}.pid"
required_dirs="${budyk_datadir}"
required_files="${budyk_config}"

command="/usr/sbin/daemon"
procname="%%PREFIX%%/bin/budyk"
command_args="-f -p ${pidfile} -u ${budyk_user} ${procname} serve --config ${budyk_config} ${budyk_flags}"

start_precmd="budyk_prestart"

budyk_prestart()
{
    if [ ! -d "${budyk_datadir}" ]; then
        install -d -o "${budyk_user}" -g "${budyk_group}" -m 0750 "${budyk_datadir}"
    fi
}

run_rc_command "$1"

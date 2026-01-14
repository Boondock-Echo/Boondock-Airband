#!/bin/sh

# PROVIDE: boondock_airband
# REQUIRE: DAEMON
# BEFORE: LOGIN
# KEYWORD: nojail shutdown

. /etc/rc.subr

name=boondock_airband
rcvar=boondock_airband_enable

command="/usr/local/bin/boondock_airband"

load_rc_config ${name}
run_rc_command "$1"

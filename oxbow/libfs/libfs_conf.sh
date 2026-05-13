#!/bin/bash
# LibFS configurations loaded as environment variables.
# This file defines default values.
# You can overwrite configuration by setting environment variable in run command.

export dirty_list_size=256 # The size of dirty list.

# You can overwrite configs using myconf.sh file which is not tracked by git.
[ -e "${LIBFS}/myconf.sh" ] && source "${LIBFS}/myconf.sh" || true

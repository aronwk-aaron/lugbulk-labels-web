#!/bin/sh
# The server runs as the unprivileged `lugbulk` user. Volumes created by
# older images (which ran as root) are root-owned, so when started as root
# hand /data over first, then drop privileges.
set -e
if [ "$(id -u)" = "0" ]; then
    chown -R lugbulk:lugbulk "${LUGBULK_DATA_DIR:-/data}"
    exec setpriv --reuid=lugbulk --regid=lugbulk --init-groups "$@"
fi
exec "$@"

#!/bin/sh

set -u

for fixture in nc_sftp_alpine nc_webdav_alpine nc_ftp_alpine; do
  docker rm -f "$fixture" >/dev/null 2>&1 || true
  docker image rm "$fixture" >/dev/null 2>&1 || true
done

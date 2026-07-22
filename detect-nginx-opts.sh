#!/usr/bin/env bash
#
# Prints to stdout the configure options of the installed nginx (detected
# from "nginx -V"), filtered to keep only the ones vanilla nginx.org's
# ./configure recognizes.
#
# Needed because distros often patch their own configure with extra
# packaging options (e.g. Debian/Ubuntu add --override-system=,
# --override-release=, --override-machine= for dpkg-buildpackage
# cross-building): valid for the distro's patched configure, but unknown
# to the one downloaded from nginx.org, which then fails with "invalid
# option". The fix here is a whitelist of only the prefixes actually
# recognized by auto/options in the official source, not a blocklist of
# known extensions (which would mean chasing every distro).
#
# Usage: detect-nginx-opts.sh [path-to-nginx-binary]

set -euo pipefail

NGINX_BIN="${1:-nginx}"

if ! command -v "$NGINX_BIN" >/dev/null 2>&1; then
    exit 0
fi

raw="$("$NGINX_BIN" -V 2>&1 | sed -n 's/^configure arguments: //p')"

if [ -z "$raw" ]; then
    exit 0
fi

# eval+set-- reconstructs the original argument array respecting the
# quoting "nginx -V" printed them with (the same technique the distro used
# to pass them to configure in the first place).
eval "set -- $raw"

out=()

for arg in "$@"; do
    case "$arg" in
        --with-*|--without-*| \
        --add-module=*|--add-dynamic-module=*| \
        --prefix=*|--sbin-path=*|--conf-path=*|--pid-path=*|--lock-path=*| \
        --user=*|--group=*|--error-log-path=*|--http-log-path=*| \
        --http-*-temp-path=*|--modules-path=*| \
        --build=*|--builddir=*|--crossbuild=*|--test-build-*)
            out+=("$arg")
            ;;
        *)
            # dropped: not recognized by the vanilla configure
            # (typically a distro packaging extension)
            ;;
    esac
done

printf '%q ' "${out[@]}"
echo

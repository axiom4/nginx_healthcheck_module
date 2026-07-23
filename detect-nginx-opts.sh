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

# Reconstruct the original argument array respecting the quoting "nginx -V"
# printed them with, without ever evaluating "$raw" as shell code: xargs
# tokenizes shell-style quoting/escaping but performs no command
# substitution, so an nginx binary with an attacker-crafted configure
# string can't execute code at build time here.
args=()
while IFS= read -r arg; do
    args+=("$arg")
done < <(xargs -n1 <<<"$raw")

out=()

for arg in "${args[@]}"; do
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

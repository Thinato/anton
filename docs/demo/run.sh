#!/bin/sh
# anton on made-up PRs and a fake Slack plugin, for screenshots.
#   ./docs/demo/run.sh        (build first; ANTON=path overrides the binary)
set -e
dir=$(cd "$(dirname "$0")" && pwd)
cfg=$(mktemp -d)
trap 'rm -rf "$cfg"' EXIT
mkdir -p "$cfg/anton"
sed "s|@DIR@|$dir|g" "$dir/config.json" > "$cfg/anton/config.json"
PATH="$dir:$PATH" XDG_CONFIG_HOME="$cfg" "${ANTON:-$dir/../../build/anton}" "$@"

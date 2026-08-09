#!/usr/bin/env bash

set -uo pipefail

if [[ "$#" -ne 1 ]]; then
    echo "[ERROR] size parser expects exactly one value" >&2
    exit 2
fi

raw_value="$1"
normalized_value="$(printf '%s' "$raw_value" | tr '[:upper:]' '[:lower:]')"
magnitude="$normalized_value"
multiplier=1

case "$normalized_value" in
    *mb)
        magnitude="${normalized_value%mb}"
        multiplier=1048576
        ;;
    *gb)
        magnitude="${normalized_value%gb}"
        multiplier=1073741824
        ;;
esac

if [[ ! "$magnitude" =~ ^[0-9]+$ ]] ||
   [[ "$normalized_value" != "$magnitude" &&
      "$normalized_value" != "${magnitude}mb" &&
      "$normalized_value" != "${magnitude}gb" ]]; then
    echo "[ERROR] invalid CASE26_PERSISTENT_BYTES='$raw_value'; expected bytes, mb, or gb (for example 5368709120, 64mb, or 5gb)" >&2
    exit 2
fi

byte_count=$((10#$magnitude * multiplier))
if [[ "$byte_count" -le 0 ]]; then
    echo "[ERROR] CASE26_PERSISTENT_BYTES must be greater than zero: '$raw_value'" >&2
    exit 2
fi

printf '%s\n' "$byte_count"

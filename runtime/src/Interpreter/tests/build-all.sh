#!/bin/bash

for dir in */; do
    # Ensure it is actually a directory
    [ -d "$dir" ] || continue

    # Strip the trailing slash
    target="${dir%/}"

    if [ "$target" == "shared" ]; then
        echo "Skipping $target..."
        continue
    fi

    echo "Starting build for: $target"
    ./build.sh "$target"
done
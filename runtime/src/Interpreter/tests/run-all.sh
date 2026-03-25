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

    echo "              Running: $target"
    if ! ./run.sh "$target"; then
        echo "Error: test '$target' failed. Exiting."
        exit 1
    fi
    echo -e "\n\n\n\n"
done

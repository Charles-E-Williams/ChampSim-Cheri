#!/bin/bash

echo "Cleaning previous builds..."
make clean

# Find all json files but explicitly exclude vcpkg.json
configs=$(ls *.json 2>/dev/null | grep -v "vcpkg.json")

if [ -z "$configs" ]; then
    echo "[ERROR] No valid configuration JSON files found."
    exit 1
fi

echo "Starting ChampSim build for all configs..."
echo ""

for config in $configs; do
    echo "JSON Config: $config"
    ./config.sh "$config"
    
    if [ $? -ne 0 ]; then
        echo "[FAILED] Configuration failed for $config. Skipping."
        continue
    fi

    make -j8

    if [ $? -eq 0 ]; then
        echo "Compiled: $config"
    else
        echo "Compilation failed for $config. Exiting"
        exit 1
    fi
    echo ""
done

echo "Done."
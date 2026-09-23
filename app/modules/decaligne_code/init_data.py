#!/bin/sh

set -e

SOURCE_DIR="/vizapi/decaligne-seed-data/cities"
TARGET_ROOT="/vizapi/data/modules/decaligne_code"
TARGET_DIR="${TARGET_ROOT}/cities"

echo "[Decaligne] Initializing data..."

mkdir -p "${TARGET_ROOT}"
mkdir -p "${TARGET_ROOT}/evaluation"
mkdir -p "${TARGET_ROOT}/runtime"

if [ ! -d "${SOURCE_DIR}" ]; then
    echo "[Decaligne] WARNING: city seed data not found: ${SOURCE_DIR}"
    exit 0
fi

# Synchronize only Decaligne public city data.
# Do not touch evaluation/, runtime/, or any other VizAPI module.
mkdir -p "${TARGET_DIR}"

cp -R "${SOURCE_DIR}/." "${TARGET_DIR}/"

echo "[Decaligne] City data ready at ${TARGET_DIR}"
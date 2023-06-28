#!/bin/bash
# Test if all the previous diskquota minor versions can be directly upgraded
# to the current version.

set -ex

SCRIPT_PATH="${BASH_SOURCE[0]}"
SRC_DIR="$(cd "$(dirname "${SCRIPT_PATH}")"/.. >/dev/null 2>&1 && pwd)"

# Versions like major.minor
CUR_VERSION=$(cut --delimiter="." --fields=1-2 "${SRC_DIR}/VERSION")
ALL_VERSIONS=$(cd "${SRC_DIR}" && git tag | cut --delimiter="." --fields=1-2 | sort -V -u)
VERSIONS_TO_TEST=()

test_alter_from() {
    local from_ver=$1
    local to_ver=$CUR_VERSION

    gpconfig -c shared_preload_libraries -v ""
    gpstop -rai
    dropdb diskquota --if-exists
    dropdb diskquota_alter_test --if-exists
    createdb diskquota

    local so_name="diskquota"
    if [ "${from_ver}" != "1.0" ];then
        so_name="diskquota-${from_ver}"
    fi

    gpconfig -c shared_preload_libraries -v "${so_name}"
    gpstop -rai

    createdb diskquota_alter_test

    # Test if the extension and be upgraded directly
    psql -d diskquota_alter_test -c "CREATE EXTENSION diskquota version '${from_ver}'"
    psql -d diskquota_alter_test -c "ALTER EXTENSION diskquota update to '${to_ver}'"
    psql -d diskquota_alter_test -c "DROP EXTENSION diskquota"
}

# Find all minor versions before current one
while IFS= read -r ver; do
    if [ "${ver}" = "${CUR_VERSION}" ]; then
        break
    fi
    if [ "${ver}" = "0.8" ]; then
        continue
    fi
    VERSIONS_TO_TEST+=("${ver}")
done <<< "$ALL_VERSIONS"

for from_ver in "${VERSIONS_TO_TEST[@]}"; do
    test_alter_from "${from_ver}"
done

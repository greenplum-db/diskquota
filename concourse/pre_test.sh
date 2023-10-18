#!/usr/bin/env bash

set -ex

# shellcheck source=/dev/null
source "$CI_REPO_DIR/common/entry_common.sh"

install_cmake

start_gpdb

## Currently, isolation2 testing framework relies on pg_isolation2_regress, we
## should build it from source. However, in concourse, the gpdb_bin is fetched
## from remote machine, the $(abs_top_srcdir) variable points to a non-existing
## location, we fixes this issue by creating a symbolic link for it.
function create_fake_gpdb_src() {
    local fake_gpdb_src
    fake_gpdb_src="$(\
        grep -rhw '/usr/local/greenplum-db-devel' -e 'abs_top_srcdir = .*' |\
        head -n 1 | awk '{ print $NF; }')"

    if [ -d "${fake_gpdb_src}" ]; then
        echo "Fake gpdb source directory has been configured."
        return
    fi

    pushd /home/gpadmin/gpdb_src
    ./configure --prefix=/usr/local/greenplum-db-devel \
        --without-zstd \
        --disable-orca --disable-gpcloud --enable-debug-extensions
    popd

    local fake_root
    fake_root=$(dirname "${fake_gpdb_src}")
    mkdir -p "${fake_root}"
    ln -s /home/gpadmin/gpdb_src "${fake_gpdb_src}"
}

create_fake_gpdb_src

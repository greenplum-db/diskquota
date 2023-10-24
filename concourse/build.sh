#!/bin/bash -l

set -exo pipefail

function pkg() {
    [ -f /opt/gcc_env.sh ] && source /opt/gcc_env.sh
    source /usr/local/greenplum-db-devel/greenplum_path.sh

    # Always use the gcc from $PATH, to avoid using a lower version compiler by /usr/bin/cc
    export CC="$(which gcc)"
    export CXX="$(which g++)"

    pushd /home/gpadmin/bin_diskquota
    cmake /home/gpadmin/diskquota_src \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    cmake --build . --target create_artifact
    popd
}

function _main() {
    time pkg
}

_main "$@"

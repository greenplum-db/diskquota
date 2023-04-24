# Run regression tests in Docker container

You can build your Docker image from GPDB source or use prebuilt images from hub.adsw.io.
How to build Docker image: (["readme.md"](https://github.com/arenadata/gpdb/blob/f7ff7c8ecae4ce7ab3b73fd46171cdaa457b3591/arenadata/readme.md)).

1. Download the cmake-3.20 install script from ([source](https://github.com/Kitware/CMake/releases/download/v3.20.0/cmake-3.20.0-linux-x86_64.sh)).

2. Build diskquota in the Docker container.
Change <PATH_TO_DISKQUOTA_SRC> and <PATH_TO_CMAKE_INSTALL_SCRIPT> to the appropriate paths on your local machine.

```
docker run --rm -it -e DISKQUOTA_OS=rhel7 \
       -v /tmp/diskquota_artifacts:/home/gpadmin/diskquota_artifacts \
       -v <PATH_TO_DISKQUOTA_SRC>:/home/gpadmin/diskquota_src \
       -v <PATH_TO_CMAKE_INSTALL_SCRIPT>:/home/gpadmin/bin_cmake/cmake-3.20.0-linux-x86_64.sh \
       hub.adsw.io/library/gpdb6_regress:latest diskquota_src/concourse/scripts/entry.sh build
```

3. Run tests.
Change <PATH_TO_DISKQUOTA_SRC> and <PATH_TO_CMAKE_INSTALL_SCRIPT> to the appropriate paths on your local machine.

```
docker run --rm -it --sysctl 'kernel.sem=500 1024000 200 4096' \
       -v /tmp/diskquota_artifacts:/home/gpadmin/bin_diskquota \
       -v <PATH_TO_DISKQUOTA_SRC>:/home/gpadmin/diskquota_src \
       -v <PATH_TO_CMAKE_INSTALL_SCRIPT>:/home/gpadmin/bin_cmake/cmake-3.20.0-linux-x86_64.sh \
       hub.adsw.io/library/gpdb6_regress:latest diskquota_src/concourse/scripts/entry.sh test
```

This file system driver is implemented using fuse low level API.

*  Install fuse library.
Download fuse library from the following link.

https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz

Untar it and build/install using following commands:

./configure
make -j8
make install

*  Install tcmalloc or remove that from Makefile.
    On ubuntu, run "sudo apt-get install libgoogle-perftools-dev"
*  Build this directory by running make.
*  Download docker sources and add files for this driver.  Build docker and install that.
*  Mount a device/file - "sudo ./dfs <device> <mnt>".
*  Stop docker and start docker with arguments "-s dfs -g <mnt>". -g argument is needed only if <mnt> is not /var/lib/docker.
*  Run experiments, stop docker, umount - "sudo fusermount -u <mnt>"
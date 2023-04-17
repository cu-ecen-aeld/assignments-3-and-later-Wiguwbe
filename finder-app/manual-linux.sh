#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u
set -x

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.1.10
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # patch for yyloc
    # wget https://raw.githubusercontent.com/bwalle/ptxdist-vetero/f1332461242e3245a47b4685bc02153160c0a1dd/patches/linux-5.0/dtc-multiple-definition.patch
    cp ${FINDER_APP_DIR}/dtc-multiple-definition.patch .
    git apply dtc-multiple-definition.patch

    # DONE: Add your kernel build steps here
    # clean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    # defconfig
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    # build
    make -j2 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

    cd ..
fi

echo "Adding the Image in outdir"
cp linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# DONE: Create necessary base directories
mkdir -p ${OUTDIR}/rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr/{bin,lib,sbin},var/log}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git --single-branch --branch ${BUSYBOX_VERSION}
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # DONE:  Configure busybox
    make distclean
    make defconfig
else
    cd busybox
fi

# Done: Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
#${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
#${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# Done: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
# readelf -l for program headers (interpreter)
interpreter=$(${CROSS_COMPILE}readelf -l ${OUTDIR}/rootfs/bin/busybox | grep -Po '(?<=program interpreter: )[^\]]+')
cp ${SYSROOT}/${interpreter} ${OUTDIR}/rootfs/${interpreter}
# readelf -d for dynamic section
for dynlib in $(${CROSS_COMPILE}readelf -d ${OUTDIR}/rootfs/bin/busybox | grep -Po '(?<=Shared library: \[)[^\]]+')
do
    cp ${SYSROOT}/lib64/${dynlib} ${OUTDIR}/rootfs/lib64/
done

# Done: Make device nodes
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1

# Done: Clean and build the writer utility
pushd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}
popd

# Done: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
cp -Lr ${FINDER_APP_DIR}/{finder.sh,finder-test.sh,writer,autorun-qemu.sh,conf} ${OUTDIR}/rootfs/home/

# Done: Chown the root directory
sudo chown -R root:root ${OUTDIR}/rootfs/*

# Done: Create initramfs.cpio.gz
pushd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
popd
cd ${OUTDIR}
gzip -f initramfs.cpio

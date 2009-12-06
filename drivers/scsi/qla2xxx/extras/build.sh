#!/bin/sh

# QLogic ISP2xxx device driver build script
# Copyright (C) 2003-2004 QLogic Corporation
# (www.qlogic.com)
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#

Q_NODE=QLA2XXX

UNAME=`uname -a`
K_VERSION=`uname -r`
K_LIBS=/lib/modules/${K_VERSION}
K_BUILD_DIR=${K_LIBS}/build
K_SOURCE_DIR=${K_LIBS}/source
K_THREADS=5

SLES=/etc/SuSE-release
RHEL=/etc/redhat-release

K_INSTALL_DIR=${K_LIBS}/extra/qlgc-qla2xxx/
if test -f "${SLES}" ; then
	K_INSTALL_DIR=${K_LIBS}/updates/
fi

BOOTDIR="/boot"

if [ "`uname -m`" = "ia64" ]; then
        if [ -f "$RHEL" ]; then
                BOOTDIR="/boot/efi/efi/redhat"
        fi
fi

###
# drv_build -- Generic 'make' command for driver
#	$1 -- directive
#
drv_build() {
	test -z "$1" && return 1

	# Go with build...
	if test -f "${SLES}" ; then
		# SuSE -------------------------------------------------------
		make -j${K_THREADS} -C ${K_SOURCE_DIR} O=${K_BUILD_DIR} M=$PWD $1
	else
		# Redhat -----------------------------------------------------
		make -j${K_THREADS} -C ${K_BUILD_DIR} M=$PWD $1
	fi
}

###
# drv_install -- Generic steps for installation
#
drv_install() {
	if test $EUID -ne 0 ; then
		echo "${Q_NODE} -- Must be root to install..."
		return 1
	fi


	#backup all modules except the one in default path
	for module in `find /lib/modules/$K_VERSION -name qla2xxx.ko`
	do
		echo $module | grep "scsi" >& /dev/null
		if [ $? -ne 0 ]; then
			mv $module $module.org
		fi
	done

	echo "${Q_NODE} -- Installing the qla2xxx modules to ${K_INSTALL_DIR}..."
	install -d -o root -g root ${K_INSTALL_DIR}
	install -o root -g root -m 0644 *.ko ${K_INSTALL_DIR}

	# depmod
	/sbin/depmod -a
}

###
#
#
case "$1" in
    install)
	# QLA2XXX Specific
	echo "${Q_NODE} -- Building the qla2xxx driver..."
	drv_build modules

	drv_install
	;;
    remove)
	# QLA2XXX Specific
	echo "${Q_NODE} -- Removing the qla2xxx driver..."
	if  [ -f ${K_INSTALL_DIR}/qla2xxx.ko ]; then
		rm ${K_INSTALL_DIR}/qla2xxx.ko
		/sbin/depmod -a
		echo "${Q_NODE} -- Rebuilding INITRD image..."
		if test -f "${SLES}" ; then
			if [ -f /sbin/mk_initrd ]; then
				/sbin/mk_initrd
			elif [ -f /sbin/mkinitrd ]; then
				/sbin/mkinitrd
			else
				echo "${Q_NODE} -- Unable to find mkinitrd command..."
				echo "${Q_NODE} -- Skipping rebuilding of INITRD image..."
			fi
					
		else
			if [ -f ${BOOTDIR}/initrd-${K_VERSION}.img ]; then
				/sbin/mkinitrd -f ${BOOTDIR}/initrd-${K_VERSION}.img ${K_VERSION}
			else
				/sbin/mkinitrd ${BOOTDIR}/initrd-${K_VERSION}.img ${K_VERSION}
			fi
		fi
	fi 
	;;

    initrd)
	# QLA2XXX Specific
	echo "${Q_NODE} -- Building the qla2xxx driver..."
	drv_build modules

	drv_install

	echo "${Q_NODE} -- Rebuilding INITRD image..."
	if test -f "${SLES}" ; then
		if [ ! -f ${BOOTDIR}/initrd-${K_VERSION}.bak ]; then
			cp ${BOOTDIR}/initrd-${K_VERSION} ${BOOTDIR}/initrd-${K_VERSION}.bak
		fi
		if [ -f /sbin/mk_initrd ]; then
			/sbin/mk_initrd
		elif [ -f /sbin/mkinitrd ]; then
			/sbin/mkinitrd
		else
			echo "${Q_NODE} -- Unable to find mkinitrd command..."
			echo "${Q_NODE} -- Skipping rebuilding of INITRD image..."
		fi
				
	else
		if [ -f ${BOOTDIR}/initrd-${K_VERSION}.img ]; then
			if [ ! -f ${BOOTDIR}/initrd-${K_VERSION}.bak.img ]; then
				cp ${BOOTDIR}/initrd-${K_VERSION}.img ${BOOTDIR}/initrd-${K_VERSION}.bak.img
			fi
			/sbin/mkinitrd -f ${BOOTDIR}/initrd-${K_VERSION}.img ${K_VERSION}
		else
			/sbin/mkinitrd ${BOOTDIR}/initrd-${K_VERSION}.img ${K_VERSION}
		fi
	fi

	;;
    clean)
	echo "${Q_NODE} -- Cleaning driver build directory..."
	drv_build clean
	;;
    new)
	echo "${Q_NODE} -- Clean rebuild of the qla2xxx driver..."
	drv_build clean
	drv_build modules
	;;
    *)
	echo "${Q_NODE} -- Building the qla2xxx driver..."
	drv_build modules
	;;
esac


#
# QLogic Fibre Channel HBA Driver
# Copyright (c)  2003-2014 QLogic Corporation
# 
# See LICENSE.qla2xxx for copyright and licensing details.
#

SCST_INC=/usr/local/include/scst
SHELL = /bin/bash

help:
	@echo "	all	: build and install drivers"
	@echo "	build	: build drivers"
	@echo "	install	: install drivers"
	@echo "	clean	: remove drivers"
	@echo "	uninstall: uninstall drivers"


all: clean
	@if [ -d $(SCST_INC) ] ; then \
		cd drivers/scsi/ && BUILD_2X_MODULE=y CONFIG_SCSI_QLA_FC=y \
		CONFIG_SCSI_QLA2XXX_TARGET=y make -s -C qla2xxx/qla2x00-target \
		all; \
	else\
		echo "Please install SCST drivers & Include file"; \
		exit 1; \
	fi
	cd drivers/scsi/ && BUILD_2X_MODULE=y CONFIG_SCSI_QLA_FC=y \
	CONFIG_SCSI_QLA2XXX_TARGET=y make -C qla2xxx/qla2x00-target install
	@echo -e  "\nAll binaries and modules installed successfully."

build:
	@if [[ -d $(SCST_INC) ]] ; then \
		cd drivers/scsi/ && BUILD_2X_MODULE=y CONFIG_SCSI_QLA_FC=y \
		CONFIG_SCSI_QLA2XXX_TARGET=y make -s -C qla2xxx/qla2x00-target \
		all; \
		echo -e  "\nAll binaries builded successfully."; \
	else \
		echo "Please install SCST drivers & Include file"; \
		exit 1; \
	fi

install:
	cd drivers/scsi/ && BUILD_2X_MODULE=y CONFIG_SCSI_QLA_FC=y \
	CONFIG_SCSI_QLA2XXX_TARGET=y make -s -C qla2xxx/qla2x00-target install
	@echo -e  "\nAll binaries and modules installed successfully."

clean:
	cd drivers/scsi/qla2xxx && make clean
	cd drivers/scsi/qla2xxx/qla2x00-target && make clean

uninstall:
	cd drivers/scsi/ && BUILD_2X_MODULE=y CONFIG_SCSI_QLA_FC=y \
	CONFIG_SCSI_QLA2XXX_TARGET=y make -C qla2xxx/qla2x00-target uninstall

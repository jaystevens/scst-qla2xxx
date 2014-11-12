#!/bin/bash

$qtrc

function parse_config
{
    if [ -f $CONF ] ; then 
	source $CONF
	if [ -z $SCSTSRCDIR ] ; then
	    NEEDSCSTDIR=1
	fi
    else
	NEEDSCSTDIR=1
	touch $CONF
    fi

    if [ $NEEDSCSTDIR -gt 0 ] ; then
	cd $LPWD

	while : ; do
	    echo "Please specify your SCST '$SCSTVERSION' version Source directory"

	    read USCSTDIR

	    if [ ! -d $USCSTDIR ] ; then 
		echo "$USCSTDIR is not valid"
	    else
		cd $USCSTDIR
		SCSTSRCDIR=`pwd`

		# check scst dir validity
		if [ ! -d $SCSTSRCDIR/scst ] ; then
		    echo "[$USCSTDIR] is not a valid SCST SRC directory"
		    continue
		fi

		echo "Saving [$SCSTSRCDIR] into config [$CONF]"

		echo "SCSTSRCDIR=$SCSTSRCDIR" >> $CONF
		break;
	    fi
	done
    fi
}

# mount scst-qla2xxx.git/drivers/scsi/qla2xxx on top of <scst dir>/qla2x00t_git/
function mount_trunk
{
    cd $SCSTSRCDIR
    LSCSTSRCDIR=`pwd`
    #cd -

    QLAGIT=$LSCSTSRCDIR/qla2x00t_git

    # mount qla source over SCST source
    [ -d $QLAGIT ] ||  mkdir -p $QLAGIT

    mount | grep -q $QLAGIT
    [ $? -ne 0 ] &&  mount -o bind $QLADIR $QLAGIT

    if [ $? -ne 0 ] ; then
	echo "Unable to mount as $USER; please enter password for \"sudo mount\""
	sudo mount -o bind $QLADIR $QLAGIT
    fi

}

function umnt_trunk
{
    umount $LSCSTSRCDIR/qla2x00t_git
    if [ $? -ne 0 ] ; then
	sudo umount $LSCSTSRCDIR/qla2x00t_git
    fi
}

# mount scst-qla2xxx.git/drivers/scsi/qla2xxx on top of <scst dir>/qla2x00t/
function mount_22x
{
    cd $QLADIR

    # rename Target Trunk code/ qla2x00t-target  qla2x00t-target-trunk
    if [ ! -d qla2x00-target-trunk ] ; then

	echo ""
	echo "NOTE: "
	echo "  the script is renaming \"Trunk\" version of Target driver"
	echo "  from: qla2x00-target   to: qla2x00-target-trunk, "
	echo "  and moving \"2.2.x\" version of Target driver"
	echo "  from: qla2x00t-target2.2.x   to: qla2x00t-target"
	echo "  for build purpose"
	echo ""
	echo "These directories will move back to their old locations"
	echo "after compilation & installation"

	echo -n "Hit any key to continue. [n] to stop: "
	read x
	if [ "$x" == "n" ] || [ "$x" == "N" ] ; then 
	    exit
	fi

	mv qla2x00-target qla2x00-target-trunk
	mv qla2x00-target.2.2.x qla2x00-target
    fi
    

    SCSTQLA=$LSCSTSRCDIR/qla2x00t

    mount | grep $SCSTQLA
    [ $? -ne 0 ] && mount -o bind $QLADIR $SCSTQLA

}

function umnt_22x
{
    umount $LSCSTSRCDIR/qla2x00t

    cd $QLADIR
    echo ""
    echo "Returning QLA Target code 2.2.x & Trunk back to original location"
    mv qla2x00-target  qla2x00-target.2.2.x
    sync; sync; sync;
    mv qla2x00-target-trunk qla2x00-target

}

function umnt
{
    if [ "$SCRIPTNAME" == "build_22x.sh" ] ; then
	umnt_22x
    else
	umnt_trunk
    fi
}

function build_dd
{

    echo ""
    echo "Attempting to build SCST & QLogic drivers."

    echo -n "Hit Any Key to continue. [n] to stop: "
    read x

    if [ "$x" == "n" ] || [ "$x" == "N" ] ; then 
	# unmount 
	umnt
	exit
    fi

    cd $LSCSTSRCDIR
    #make clean

    make scst
    if [ $? -ne 0 ] ; then
	echo "SCST build failed."
	umnt
	exit
    fi

    make qla
    if [ $? -ne 0 ] ; then
	echo "QLOGIC Drivers build failed."
	umnt
	exit
    fi
}


function install_dd
{
    echo ""
    echo ""
    echo "Attempting to install SCST & QLogic drivers."

    echo -n "Hit Any Key to continue. [n] to stop: "
    read x

    if [ "$x" == "n" ] || [ "$x" == "N" ] ; then 
	umnt
	exit
    fi

    [ -f /usr/local/sbin/scstadmin ] || make scstadm_install  >> /dev/null

    make scst_install
    if [ $? -ne 0 ] ; then
	echo "SCST installation failed."
	umnt
	exit
    fi


    make qla_install
    if [ $? -ne 0 ] ; then
	echo "QLOGIC drivers installation failed."
	umnt
	exit
    fi
}

########################
# main
########################

LPWD=`pwd`

# find base directory
#echo $0
SCRIPTNAME=`basename $0`

# walk from build.sh location back to the root of scst-qla2xxx.git
# to get dir name
QLASRCDIR=`dirname $0`
cd $QLASRCDIR
QLAEXTRAS=`pwd`
cd ../../../..
QLASRCDIR=`pwd`


QLADIR="$QLASRCDIR/drivers/scsi/qla2xxx"
cd - >> /dev/null


CONF="$QLAEXTRAS/.build_config_Trunk"
SCSTVERSION='Trunk'

LSCSTSRCDIR=''

NEEDSCSTDIR=0


parse_config

cd $SCSTSRCDIR
LSCSTSRCDIR=`pwd`

echo "Using SCST SRC [$LSCSTSRCDIR]"


mount_trunk
build_dd
install_dd
umnt_trunk

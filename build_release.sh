#/bin/sh
#
# build_release.sh
#
# Builds release packages based on the latest sources from the rh6 and sp1
# branches

scratch_dir="/tmp"
branches="rh6 sp1"

# Assume we're at the root of the qla2xxx-v2632-devel tree
orig_dir=$PWD

# Create branch specific release packages

for branch in $branches 
do
	git checkout $branch
	cd drivers/scsi/qla2xxx
	drv_version="`grep QLA2XXX_VERSION qla_version.h | awk '{print $3}' | sed 's/\"//g'`"

	# Tag this release
	if [ "$branch" = "rh6" ]
	then
		git tag -m "RHEL 6 driver version $drv_version." -a "$drv_version"
	elif [ "$branch" = "sp1" ]
	then
		git tag -m "SLES 11 SP1 driver version $drv_version." -a "$drv_version"
	fi

	rm -fr $scratch_dir/qla2xxx-$drv_version $scratch_dir/qla2xxx-src-v$drv_version.tar.gz
	mkdir $scratch_dir/qla2xxx-$drv_version
	cp -r * $scratch_dir/qla2xxx-$drv_version
	cd $scratch_dir
	tar cvf qla2xxx-src-v$drv_version.tar qla2xxx-$drv_version
	gzip qla2xxx-src-v$drv_version.tar
	cd $orig_dir
done

#/bin/sh
#
# build_release.sh
#
# Builds release packages based on the latest sources from the rh6 and sp1
# branches

scratch_dir="/tmp"
orig_dir=$PWD
git_tag=1
branches=""

# All branches are passed in at the command line
for arg in $@
do
	case $arg in
	-nt|--notag) git_tag=0;; # Don't git tag this branch
	*) branches="${branches} $arg";;
	esac
done

# Create branch specific release packages

for branch in $branches 
do
	git checkout $branch
	if [ $? -ne 0 ]
	then
		continue
	fi
	cd drivers/scsi/qla2xxx
	drv_version="`grep QLA2XXX_VERSION qla_version.h | awk '{print $3}' | sed 's/\"//g'`"

	# Tag this release
	if [ $git_tag -eq 1 ]
	then
		git tag $drv_version
	fi

	rm -fr $scratch_dir/qla2xxx-$drv_version $scratch_dir/qla2xxx-src-v$drv_version.tar.gz
	mkdir $scratch_dir/qla2xxx-$drv_version
	cp -r * $scratch_dir/qla2xxx-$drv_version
	cd $scratch_dir
	tar cvf qla2xxx-src-v$drv_version.tar qla2xxx-$drv_version
	gzip qla2xxx-src-v$drv_version.tar
	cd $orig_dir
done

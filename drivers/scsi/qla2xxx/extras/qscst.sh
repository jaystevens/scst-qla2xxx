#!/bin/sh
#
# QLogic ISP2xxx device driver debug level tuning tool.
# Copyright (C) 2012 QLogic Corporation
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
# qscst.sh  : QLogic diagnostic script to capture relevant SCST debug info.
#
  version="1.00"
#
# qscst.sh
#   -C : collect stats.
#

function exit_pgm()
{
    local status msg
    status=$1
    msg=${2-""}
    if [[ "$msg" != "" ]];then
        echo -e "ERROR: $msg\n" >&2
    fi
    if [[ "$status" -eq 2 ]];then
        cat <<- 'EOF' >&2
Usage: qscst.sh [-C]
    C : Diagnostic information collection interface (default).
EOF
    fi
    exit $status
}

function log()
{
    local level msg
    level=$1
    msg=${2-""}
    if [[ $opt_v -ge $level ]];then
        echo -e "$msg"
    fi
}

# Usage: tunable_enabled <name>
function tunable_enabled()
{
    local tunable
    tunable=$1
    eval "let QLA_TUNABLE__$tunable=1"
}

# Usage: tunable_check <name>
function tunable_check()
{
    local tunable check
    tunable=$1
    eval "let check=\${QLA_TUNABLE__$tunable-0}"
    # shell uses 0 as success
    return $(($check!=1))
}

function qla_dfs_check()
{
    local qdir_dfs
    if [[ ! -d $QLA_DEBUG_FS ]];then
        log 1 "No $QLA_DEBUG_FS."
        return 1
    fi
    return 0
}

function qla_dfs_empty()
{
    qla_dfs_check || return 1
    if [[ $(ls $QLA_DEBUG_FS |grep -c ^) -eq 0 ]];then
        log 1 "Empty $qdir_dfs."
        return 1
    fi
    return 0
}

# Q rate tunable related (
function enable_qrate()
{
    # check if user has requested to turn qrate on first.
    tunable_check "qrate_usr" || return
    log 2 " --> Enabling queue rate tunables.."
    if [[ ! -f $QLA_PARM_DIR/qla_q_rate ]];then
        log 1 "No qla_q_rate tunable."
        return
    fi
    echo 1 > $QLA_PARM_DIR/qla_q_rate
    [[ $? -eq 0 ]] && tunable_enabled "qrate"
}

function collect_qrate_stats()
{
    tunable_check "qrate" || return
    qla_dfs_empty || return
    log 2 " --> Collecting q_rate stat.."
    for f in $QLA_DEBUG_FS/*; do
        if [[ ! -f $f/q_rate_hist ]];then
            log 1 "No q_rate_hist file."
            return
        fi
        cat $f/q_rate_hist > q_rate_hist.${f##*_}
    done
}

function clear_qrate_stats()
{
    local qdir
    tunable_check "qrate" || return
    qla_dfs_empty || return
    log 2 "  --> Clearing q_rate stat.."
    for f in $QLA_DEBUG_FS/*; do
        if [[ ! -f $f/q_rate_hist ]];then
            log 1 "No q_rate_hist file."
            return
        fi
        echo 0 > $f/q_rate_hist
    done
}
# Q rate tunable related )

function clear_stats()
{
    log 1 "  -> Clearing stats now.."
    clear_qrate_stats
}

function collect_stats()
{
    log 1 "  -> Colletcting stats now.."
    collect_qrate_stats
}

function enable_tunables()
{
    log 1 " -> Enabling tunables.."

    # As we have only 1 tunable (qrate) now, turn it on by default, when more
    # tunables appear, a user dialog can turn on appropriate ones.
    tunable_enabled "qrate_usr"
    enable_qrate
}

function run_diag()
{
    local dirn rsp run rund pwd
    pwd=$PWD
    trap 'cd $pwd;echo;return' 2
    echo "Hit Ctrl-C anytime to stop."

    log 0 "Enabling tunables.."
    enable_tunables
    let run=0
    while true; do
        rund=run$run
        mkdir $rund
        \cd $rund
        echo
        log 0 "Clearing stats.."
        clear_stats
        read -p "1. Please enter a test description: " rsp
        [[ "$rsp" != "" ]] && echo "$rsp" > README.txt
        read -p "2. Start tests now, hit ENTER when done." rsp
        collect_stats
        read -p "3. Hit ENTER to continue with more runs, 'q' to quit: " rsp
        let run=run+1
        \cd ..
        [[ "$rsp" = "q" ]] && break
    done
    if [[ ! -f $QDIAG ]];then
        log 0 "WARNING: $QDIAG not found"
        return
    fi
    log 0 "Collecting system information.."
    echo "qscst.sh version: $version" > version.txt
    scstadmin -write_config scst_config.txt >/dev/null 2>&1
    $QDIAG -d. > /dev/null
}

function read_opts()
{
    local o

    while getopts vC o; do
        case "$o" in
        #a)  arg="$OPTARG";;
        v)  opt_v=1 ;;
        C)  opt_C=1 ;;
        *)  exit_pgm 2 ;;
        esac
    done
    shift $((OPTIND-1))
}

function script_init()
{
    mkdir -p $OUT_DIR
    [[ $? -ne 0 ]] && exit_pgm 1 "Cannot create directory '$OUT_DIR'."
    cd $OUT_DIR

    if [[ ! -d $DEBUG_FS_DIR ]]; then
        mount -t debugfs none $DEBUG_FS_DIR
    fi
}

function zip_all()
{
    \cd ..
    if [[ ! -d $OUT_DIR ]];then
        log 1 "No $OUT_DIR to zip."
        return
    fi
    if [[ $(find $OUT_DIR -type f|grep -c ^) -eq 0 ]];then
        # empty directory
        log 1 "Cleaning up empty directory $OUT_DIR."
        rm -rf $OUT_DIR
        return
    fi
    OUT_FILE="$OUT_DIR.tar.gz"
    tar zcf $OUT_FILE $OUT_DIR
    rm -rf $OUT_DIR
    echo -e "\nCreated $OUT_FILE."
}

QDIR=$(\cd ${0%/*};pwd)
DEBUG_FS_DIR="/sys/kernel/debug"
QLA_DEBUG_FS="$DEBUG_FS_DIR/qla2xxx"
QLA_PARM_DIR="/sys/module/qla2xxx_scst/parameters"
QDIAG="$QDIR/qdiag.sh"
OUT_DIR=qscst-$(date '+%Y_%m_%d-%H_%M_%S')

[[ ! -a $QDIAG ]] && exit_pgm 1 "Cannot find $QDIAG."

let opt_v=opt_C=0

read_opts "${@--C}"

script_init

[[ "$opt_C" -eq 1 ]] && run_diag

zip_all

exit 0

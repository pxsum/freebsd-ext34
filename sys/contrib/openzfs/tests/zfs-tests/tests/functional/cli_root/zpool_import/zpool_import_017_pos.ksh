#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2012, 2016 by Delphix. All rights reserved.
# Copyright (c) 2020 Lawrence Livermore National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zpool_import/zpool_import.cfg

#
# DESCRIPTION:
#	For draid2, two destroyed pool's devices were removed or used by other
#	pool, it still can be imported correctly.
#
# STRATEGY:
#	1. Create a draid2 pool A with N disks.
#	2. Destroy this pool A.
#	3. Create another pool B with two disks which were used by pool A.
#	4. Verify import this draid2 pool can succeed.
#

verify_runnable "global"

function cleanup
{
	destroy_pool $TESTPOOL2
	destroy_pool $TESTPOOL1

	log_must rm -rf $DEVICE_DIR/*
	typeset i=0
	while (( i < $MAX_NUM )); do
		log_must mkfile $FILE_SIZE ${DEVICE_DIR}/${DEVICE_FILE}$i
		((i += 1))
	done
}

log_assert "For draid2, two destroyed pools devices was removed or used by " \
	"other pool, it still can be imported correctly."
log_onexit cleanup

log_must zpool create $TESTPOOL1 draid2 $VDEV0 $VDEV1 $VDEV2 $VDEV3
typeset guid=$(get_config $TESTPOOL1 pool_guid)
typeset target=$TESTPOOL1
if (( RANDOM % 2 == 0 )) ; then
	target=$guid
	log_note "Import by guid."
fi
log_must zpool destroy $TESTPOOL1

log_must zpool create $TESTPOOL2 $VDEV0 $VDEV1
log_must zpool import -d $DEVICE_DIR -D -f $target
log_must zpool destroy $TESTPOOL1

log_must zpool destroy $TESTPOOL2
log_must rm -rf $VDEV0 $VDEV1
log_must zpool import -d $DEVICE_DIR -D -f $target
log_must zpool destroy $TESTPOOL1

log_note "For draid2, more than two destroyed pool's devices were used, " \
	"import failed."
log_must mkfile $FILE_SIZE $VDEV0 $VDEV1
log_must zpool create $TESTPOOL2 $VDEV0 $VDEV1 $VDEV2
log_mustnot zpool import -d $DEVICE_DIR -D -f $target
log_must zpool destroy $TESTPOOL2

log_pass "zpool import -D draid2 passed."

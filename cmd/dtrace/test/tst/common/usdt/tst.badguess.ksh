#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
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
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

if [ $# != 1 ]; then
	echo expected one argument: '<'dtrace-path'>'
	exit 2
fi

dtrace=$1
DIR=/var/tmp/dtest.$$

mkdir $DIR
cd $DIR

cat > prov.d <<EOF
provider test_prov {
	probe go();
};
EOF

$dtrace -h -s prov.d
if [ $? -ne 0 ]; then
	print -u2 "failed to generate header file"
	exit 1
fi

cat > test.c <<EOF
#include <sys/types.h>
#include "prov.h"

int
main(int argc, char **argv)
{
	if (TEST_PROV_GO_ENABLED()) {
		TEST_PROV_GO();
	}
}
EOF

gcc -m64 -c -o test64.o test.c
if [ $? -ne 0 ]; then
	print -u2 "failed to compile test.c 64-bit"
	exit 1
fi
gcc -m32 -c -o test32.o test.c
if [ $? -ne 0 ]; then
	print -u2 "failed to compile test.c 32-bit"
	exit 1
fi

$dtrace -G -s prov.d test32.o test64.o
if [ $? -eq 0 ]; then
	print -u2 "DOF generation failed to generate a warning"
	exit 1
fi

cd /
/usr/bin/rm -rf $DIR

exit 0

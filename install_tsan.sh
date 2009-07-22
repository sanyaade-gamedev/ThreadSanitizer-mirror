#!/bin/bash
# Script to add ThreadSanitizer into existing valgrind directory

VALGRIND_REV=10454
TSAN_REV=1089
VALGRIND_DIR=`pwd`

if ! svn info | grep "Revision: ${VALGRIND_REV}" >/dev/null
then
  echo "WARNING:" >&2
  echo "Either you run the script from outside of the valgrind directory" >&2
  echo "or the revision number is different the one we used (r${VALGRIND_REV})." >&2
  echo "The patch may not apply" >&2
fi

set -x
set -e

cd ..
# Get ThreadSanitizer into directory 'tsan' and put it next to your valgrind directory.
svn checkout -r $TSAN_REV http://data-race-test.googlecode.com/svn/trunk/tsan tsan

cd $VALGRIND_DIR
# Create symlinks of tsan files (and some dummy files) in valgrind directory
mkdir tsan
cd tsan
ln -s ../../tsan/[A-Za-z]* .

# Create empty 'docs' and 'tests' directories - valgrind needs them.
mkdir docs tests
touch docs/Makefile.am tests/Makefile.am

# Apply a patch to the valgrind files.
cd ../
patch -p 0 < tsan/valgrind.patch

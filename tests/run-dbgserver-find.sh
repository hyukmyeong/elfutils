# Copyright (C) 2019 Red Hat, Inc.
# This file is part of elfutils.
#
# This file is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# elfutils is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

. $srcdir/test-subr.sh


if [ -z $DEBUGINFO_SERVER ]; then
  echo "unknown server url"
  exit 77
fi

testfiles testfile-dbgserver.exec
testfiles testfile-dbgserver.debug

DB="$PWD/.dbgserver_tmp.sqlite"

if [ -z $HOME ]; then
  CACHE_DIR="/.dbgserver_client_cache"
else
  CACHE_DIR="$HOME/.dbgserver_client_cache"
fi

# Clear the cache
if [ -d $CACHE_DIR ]; then
  echo "t"
  rm -rf $CACHE_DIR
fi

../../src/dbgserver -vvv -d $DB -F testfile-dbgserver.debug &
PID=$!
sleep 5

# Test whether the server is able to fetch the file from the local dbgserver.
testrun ${abs_builddir}/dbgserver_build_id_find -e testfile-dbgserver.exec

kill $PID
rm $DB

# Run the test again without the server running. The target file should
# be found in the cache.
testrun ${abs_builddir}/dbgserver_build_id_find -e testfile-dbgserver.exec

rm -rf $CACHE_DIR

exit 0

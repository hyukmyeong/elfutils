#!/bin/gawk -f

## Copyright (C) 2012 Red Hat, Inc.
##
## This file is part of elfutils.
##
## This file is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 3 of the License, or
## (at your option) any later version.
##
## elfutils is distributed in the hope that it will be useful, but
## WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.

$1 == "enum" { set = ""; next }

set == "" && $1 ~ /DW_([A-Z_]+)_([^ ]+)/ {
  set = $1;
  sub(/^DW_/, "", set);
  sub(/_[^[:upper:]_].*$/, "", set);
  if (set ~ /LANG_.+/) set = "LANG";
}

$1 ~ /DW([_A-Z]+)_([^ ]+)/ {
  match($1, ("DW_" set "_([^ ]+)"), fields);
  elt = fields[1];
  if (set in DW)
    DW[set] = DW[set] "," elt;
  else
    DW[set] = elt;
  if ($NF == "*/" && $4 == "/*") {
    c = $5;
    for (i = 6; i < NF; ++i) c = c " " $i;
    comment[set, elt] = c;
  }
}

END {
  print "/* Generated by config/dwarf-known.awk from libdw.h contents.  */";
  n = asorti(DW, sets);
  for (i = 1; i <= n; ++i) {
    set = sets[i];
    if (what && what != set) continue;
    split(DW[set], elts, ",");
    m = asort(elts);
    lo = hi = "";
    if (m == 0) continue;
    print "\n#define ALL_KNOWN_DW_" set " \\";
    for (j = 1; j <= m; ++j) {
      elt = elts[j];
      if (elt ~ /(lo|low)_user$/) {
	lo = elt;
	continue;
      }
      if (elt ~ /(hi|high)_user$/) {
	hi = elt;
	continue;
      }
      if (comment[set, elt])
	print "  ONE_KNOWN_DW_" set "_DESC (" elt ", DW_" set "_" elt \
	  ", \"" comment[set, elt] "\") \\";
      else
	print "  ONE_KNOWN_DW_" set " (" elt ", DW_" set "_" elt ") \\";
    }
    print "  /* End of DW_" set "_*.  */";
  }
}

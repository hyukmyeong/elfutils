/* Pedantic checking of DWARF files.
   Copyright (C) 2011 Red Hat, Inc.
   This file is part of Red Hat elfutils.

   Red Hat elfutils is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 2 of the License.

   Red Hat elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with Red Hat elfutils; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301 USA.

   Red Hat elfutils is an included package of the Open Invention Network.
   An included package of the Open Invention Network is a package for which
   Open Invention Network licensees cross-license their patents.  No patent
   license is granted, either expressly or impliedly, by designation as an
   included package.  Should you wish to participate in the Open Invention
   Network licensing program, please visit www.openinventionnetwork.com
   <http://www.openinventionnetwork.com>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "highlevel_check.hh"
#include "../src/dwarfstrings.h"
#include "all-dies-it.hh"
#include "pri.hh"
#include "messages.hh"
#include <map>

using elfutils::dwarf;

namespace
{
  class check_self_referential_die
    : public highlevel_check<check_self_referential_die>
  {
  public:
    static checkdescriptor const *descriptor ()
    {
      static checkdescriptor cd
	(checkdescriptor::create ("check_self_referential_die")
	 .inherit<highlevel_check<check_self_referential_die> > ()
	 .description (
"A reference attribute referencing the DIE itself is suspicious.\n"
"One example is a DW_AT_containing_type pointing to itself.\n"
" https://fedorahosted.org/pipermail/elfutils-devel/2011-February/001794.html\n"
		       ));
      return &cd;
    }

    explicit check_self_referential_die (checkstack &stack, dwarflint &lint)
      : highlevel_check<check_self_referential_die> (stack, lint)
    {
      for (all_dies_iterator<dwarf> it = all_dies_iterator<dwarf> (dw);
	   it != all_dies_iterator<dwarf> (); ++it)
	{
	  dwarf::debug_info_entry const &die = *it;
	  for (dwarf::debug_info_entry::attributes_type::const_iterator
		 at = die.attributes ().begin ();
	       at != die.attributes ().end (); ++at)
	    {
	      dwarf::attr_value const &val = (*at).second;
	      if (val.what_space () == dwarf::VS_reference)
		{
		  dwarf::debug_info_entry ref = *val.reference ();
		  if (ref.identity () == die.identity ())
		    wr_message (to_where (die),
				mc_impact_3 | mc_acc_suboptimal | mc_die_rel)
		      << "attribute " << dwarf::attributes::name ((*at).first)
		      << " references DIE itself." << std::endl;
		}
	    }
	}
    }
  };

  reg<check_self_referential_die> reg;
}

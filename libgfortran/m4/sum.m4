`/* Implementation of the SUM intrinsic
   Copyright (C) 2002-2025 Free Software Foundation, Inc.
   Contributed by Paul Brook <paul@nowt.org>

This file is part of the GNU Fortran 95 runtime library (libgfortran).

Libgfortran is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public
License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

Libgfortran is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

#include "libgfortran.h"'

include(iparm.m4)dnl
ifelse(index(rtype_name,`GFC_INTEGER'),`0',dnl
define(`rtype_name',patsubst(rtype_name,`GFC_INTEGER',`GFC_UINTEGER'))dnl
define(`atype_name',patsubst(rtype_name,`GFC_INTEGER',`GFC_UINTEGER'))dnl
define(`rtype',patsubst(rtype,`gfc_array_i',`gfc_array_m'))dnl
define(`atype',patsubst(rtype,`gfc_array_i',`gfc_array_m')))dnl
include(ifunction.m4)dnl

`#if defined (HAVE_'atype_name`) && defined (HAVE_'rtype_name`)'

ARRAY_FUNCTION(0,
`  result = 0;',
`  result += *src;')

MASKED_ARRAY_FUNCTION(0,
`  result = 0;',
`  if (*msrc)
    result += *src;')

SCALAR_ARRAY_FUNCTION(0)

#endif

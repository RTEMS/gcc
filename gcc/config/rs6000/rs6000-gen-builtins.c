/* Generate built-in function initialization and recognition for Power.
   Copyright (C) 2020 Free Software Foundation, Inc.
   Contributed by Bill Schmidt, IBM <wschmidt@linux.ibm.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* This program generates built-in function initialization and
   recognition code for Power targets, based on text files that
   describe the built-in functions and vector overloads:

     rs6000-builtin-new.def     Table of built-in functions
     rs6000-overload.def        Table of overload functions

   Both files group similar functions together in "stanzas," as
   described below.

   Each stanza in the built-in function file starts with a line
   identifying the circumstances in which the group of functions is
   permitted, with the gating predicate in square brackets.  For
   example, this could be

     [altivec]

   or it could be

     [power9]

   The bracketed gating predicate is the only information allowed on
   the stanza header line, other than whitespace.

   Following the stanza header are two lines for each function: the
   prototype line and the attributes line.  The prototype line has
   this format, where the square brackets indicate optional
   information and angle brackets indicate required information:

     [kind] <return-type> <bif-name> (<argument-list>);

   Here [kind] can be one of "const", "pure", or "fpmath";
   <return-type> is a legal type for a built-in function result;
   <bif-name> is the name by which the function can be called;
   and <argument-list> is a comma-separated list of legal types
   for built-in function arguments.  The argument list may be
   empty, but the parentheses and semicolon are required.

   The attributes line looks like this:

     <bif-id> <bif-pattern> {<attribute-list>}

   Here <bif-id> is a unique internal identifier for the built-in
   function that will be used as part of an enumeration of all
   built-in functions; <bif-pattern> is the define_expand or
   define_insn that will be invoked when the call is expanded;
   and <attribute-list> is a comma-separated list of special
   conditions that apply to the built-in function.  The attribute
   list may be empty, but the braces are required.

   Attributes are strings, such as these:

     init     Process as a vec_init function
     set      Process as a vec_set function
     extract  Process as a vec_extract function
     nosoft   Not valid with -msoft-float
     ldvec    Needs special handling for vec_ld semantics
     stvec    Needs special handling for vec_st semantics
     reve     Needs special handling for element reversal
     pred     Needs special handling for comparison predicates
     htm      Needs special handling for transactional memory
     htmspr   HTM function using an SPR
     htmcr    HTM function using a CR
     mma      Needs special handling for MMA instructions
     no32bit  Not valid for TARGET_32BIT
     cpu      This is a "cpu_is" or "cpu_supports" builtin
     ldstmask Altivec mask for load or store

   An example stanza might look like this:

[altivec]
  const vsc __builtin_altivec_abs_v16qi (vsc);
    ABS_V16QI absv16qi2 {}
  const vss __builtin_altivec_abs_v8hi (vss);
    ABS_V8HI absv8hi2 {}

   Here "vsc" and "vss" are shorthand for "vector signed char" and
   "vector signed short" to shorten line lengths and improve readability.
   Note the use of indentation, which is recommended but not required.

   The overload file has more complex stanza headers.  Here the stanza
   represents all functions with the same overloaded function name:

     [<overload-id>, <abi-name>, <builtin-name>]

   Here the square brackets are part of the syntax, <overload-id> is a
   unique internal identifier for the overload that will be used as part
   of an enumeration of all overloaded functions; <abi-name> is the name
   that will appear as a #define in altivec.h; and <builtin-name> is the
   name that is overloaded in the back end.

   Each function entry again has two lines.  The first line is again a
   prototype line (this time without [kind]):

     <return-type> <internal-name> (<argument-list>);

   The second line contains only one token: the <bif-id> that this
   particular instance of the overloaded function maps to.  It must
   match a token that appears in the bif file.

   An example stanza might look like this:

[VEC_ABS, vec_abs, __builtin_vec_abs]
  vsc __builtin_vec_abs (vsc);
    ABS_V16QI
  vss __builtin_vec_abs (vss);
    ABS_V8HI

  Blank lines may be used as desired in these files between the lines as
  defined above; that is, you can introduce as many extra newlines as you
  like after a required newline, but nowhere else.  Lines beginning with
  a semicolon are also treated as blank lines.  */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "rbtree.h"

/* Used as a sentinel for range constraints on integer fields.  No field can
   be 32 bits wide, so this is a safe sentinel value.  */
#define MININT INT32_MIN

/* Input and output file descriptors and pathnames.  */
static FILE *bif_file;
static FILE *ovld_file;
static FILE *header_file;
static FILE *init_file;
static FILE *defines_file;

static const char *pgm_path;
static const char *bif_path;
static const char *ovld_path;
static const char *header_path;
static const char *init_path;
static const char *defines_path;

/* Position information.  Note that "pos" is zero-indexed, but users
   expect one-indexed column information, so representations of "pos"
   as columns in diagnostic messages must be adjusted.  */
#define LINELEN 1024
static char linebuf[LINELEN];
static int line;
static int pos;

/* Used to determine whether a type can be void (only return types).  */
enum void_status {
  VOID_NOTOK,
  VOID_OK
};

/* Stanzas are groupings of built-in functions and overloads by some
   common feature/attribute.  These definitions are for built-in function
   stanzas.  */
enum bif_stanza {
  BSTZ_ALWAYS,
  BSTZ_P5,
  BSTZ_P6,
  BSTZ_ALTIVEC,
  BSTZ_VSX,
  BSTZ_P7,
  BSTZ_P7_64,
  BSTZ_P8,
  BSTZ_P8V,
  BSTZ_P9,
  BSTZ_P9_64,
  BSTZ_P9V,
  BSTZ_IEEE128_HW,
  BSTZ_DFP,
  BSTZ_CRYPTO,
  BSTZ_HTM,
  BSTZ_P10,
  BSTZ_MMA,
  NUMBIFSTANZAS
};

static bif_stanza curr_bif_stanza;

struct stanza_entry
{
  const char *stanza_name;
  bif_stanza stanza;
};

static stanza_entry stanza_map[NUMBIFSTANZAS] =
  {
    { "always",		BSTZ_ALWAYS	},
    { "power5",		BSTZ_P5		},
    { "power6",		BSTZ_P6		},
    { "altivec",	BSTZ_ALTIVEC	},
    { "vsx",		BSTZ_VSX	},
    { "power7",		BSTZ_P7		},
    { "power7-64",	BSTZ_P7_64	},
    { "power8",		BSTZ_P8		},
    { "power8-vector",	BSTZ_P8V	},
    { "power9",		BSTZ_P9		},
    { "power9-64",	BSTZ_P9_64	},
    { "power9-vector",	BSTZ_P9V	},
    { "ieee128-hw",	BSTZ_IEEE128_HW	},
    { "dfp",		BSTZ_DFP	},
    { "crypto",		BSTZ_CRYPTO	},
    { "htm",		BSTZ_HTM	},
    { "power10",	BSTZ_P10	},
    { "mma",		BSTZ_MMA	}
  };

static const char *enable_string[NUMBIFSTANZAS] =
  {
    "ENB_ALWAYS",
    "ENB_P5",
    "ENB_P6",
    "ENB_ALTIVEC",
    "ENB_VSX",
    "ENB_P7",
    "ENB_P7_64",
    "ENB_P8",
    "ENB_P8V",
    "ENB_P9",
    "ENB_P9_64",
    "ENB_P9V",
    "ENB_IEEE128_HW",
    "ENB_DFP",
    "ENB_CRYPTO",
    "ENB_HTM",
    "ENB_P10",
    "ENB_MMA"
  };

/* Function modifiers provide special handling for const, pure, and fpmath
   functions.  These are mutually exclusive, and therefore kept separate
   from other bif attributes.  */
enum fnkinds {
  FNK_NONE,
  FNK_CONST,
  FNK_PURE,
  FNK_FPMATH
};

/* Legal base types for an argument or return type.  */
enum basetype {
  BT_CHAR,
  BT_SHORT,
  BT_INT,
  BT_LONGLONG,
  BT_FLOAT,
  BT_DOUBLE,
  BT_INT128,
  BT_FLOAT128,
  BT_DECIMAL32,
  BT_DECIMAL64,
  BT_DECIMAL128,
  BT_IBM128
};

/* Ways in which a const int value can be restricted.  RES_BITS indicates
   that the integer is restricted to val1 bits, interpreted as an unsigned
   number.  RES_RANGE indicates that the integer is restricted to values
   between val1 and val2, inclusive.  RES_VAR_RANGE is like RES_RANGE, but
   the argument may be variable, so it can only be checked if it is constant.
   RES_VALUES indicates that the integer must have one of the values val1
   or val2.  */
enum restriction {
  RES_NONE,
  RES_BITS,
  RES_RANGE,
  RES_VAR_RANGE,
  RES_VALUES
};

/* Type modifiers for an argument or return type.  */
struct typeinfo {
  char isvoid;
  char isconst;
  char isvector;
  char issigned;
  char isunsigned;
  char isbool;
  char ispixel;
  char ispointer;
  char isopaque;
  basetype base;
  restriction restr;
  int val1;
  int val2;
};

/* A list of argument types.  */
struct typelist {
  typeinfo info;
  typelist *next;
};

/* Attributes of a builtin function.  */
struct attrinfo {
  char isinit;
  char isset;
  char isextract;
  char isnosoft;
  char isldvec;
  char isstvec;
  char isreve;
  char ispred;
  char ishtm;
  char ishtmspr;
  char ishtmcr;
  char ismma;
  char isno32bit;
  char iscpu;
  char isldstmask;
};

/* Fields associated with a function prototype (bif or overload).  */
struct prototype {
  typeinfo rettype;
  char *bifname;
  int nargs;
  typelist *args;
  int restr_opnd[2];
  restriction restr[2];
  int restr_val1[2];
  int restr_val2[2];
};

/* Data associated with a builtin function, and a table of such data.  */
#define MAXBIFS 16384
struct bifdata {
  int stanza;
  fnkinds kind;
  prototype proto;
  char *idname;
  char *patname;
  attrinfo attrs;
  char *fndecl;
};

static bifdata bifs[MAXBIFS];
static int num_bifs;
static int curr_bif;

/* Stanzas are groupings of built-in functions and overloads by some
   common feature/attribute.  These definitions are for overload stanzas.  */
struct ovld_stanza {
  char *stanza_id;
  char *extern_name;
  char *intern_name;
};

#define MAXOVLDSTANZAS 256
static ovld_stanza ovld_stanzas[MAXOVLDSTANZAS];
static int num_ovld_stanzas;
static int curr_ovld_stanza;

#define MAXOVLDS 16384
struct ovlddata {
  int stanza;
  prototype proto;
  char *idname;
  char *fndecl;
};

static ovlddata ovlds[MAXOVLDS];
static int num_ovlds;
static int curr_ovld;

/* Exit codes for the shell.  */
enum exit_codes {
  EC_OK,
  EC_BADARGS,
  EC_NOBIF,
  EC_NOOVLD,
  EC_NOHDR,
  EC_NOINIT,
  EC_NODEFINES,
  EC_PARSEBIF,
  EC_PARSEOVLD,
  EC_WRITEHDR,
  EC_WRITEINIT,
  EC_WRITEDEFINES,
  EC_INTERR
};

/* Return codes for parsing routines.  */
enum parse_codes {
  PC_OK,
  PC_EOFILE,
  PC_EOSTANZA,
  PC_PARSEFAIL
};

/* The red-black trees for built-in function identifiers, built-in
   overload identifiers, and function type descriptors.  */
static rbt_strings bif_rbt;
static rbt_strings ovld_rbt;
static rbt_strings fntype_rbt;

/* Pointer to a diagnostic function.  */
void (*diag) (const char *, ...) __attribute__ ((format (printf, 1, 2)))
  = NULL;

/* Custom diagnostics.  */
static void __attribute__ ((format (printf, 1, 2)))
bif_diag (const char * fmt, ...)
{
  va_list args;
  fprintf (stderr, "%s:%d: ", bif_path, line);
  va_start (args, fmt);
  vfprintf (stderr, fmt, args);
  va_end (args);
}

static void __attribute__ ((format (printf, 1, 2)))
ovld_diag (const char * fmt, ...)
{
  va_list args;
  fprintf (stderr, "%s:%d: ", ovld_path, line);
  va_start (args, fmt);
  vfprintf (stderr, fmt, args);
  va_end (args);
}

/* Pass over unprintable characters and whitespace (other than a newline,
   which terminates the scan).  */
static void
consume_whitespace ()
{
  while (pos < LINELEN && isspace(linebuf[pos]) && linebuf[pos] != '\n')
    pos++;
  return;
}

/* Get the next nonblank, noncomment line, returning 0 on EOF, 1 otherwise.  */
static int
advance_line (FILE *file)
{
  while (1)
    {
      /* Read ahead one line and check for EOF.  */
      if (!fgets (linebuf, sizeof(linebuf), file))
	return 0;
      line++;
      pos = 0;
      consume_whitespace ();
      if (linebuf[pos] != '\n' && linebuf[pos] != ';')
	return 1;
    }
}

static inline void
safe_inc_pos ()
{
  if (pos++ >= LINELEN)
    {
      (*diag) ("line length overrun.\n");
      exit (EC_INTERR);
    }
}

/* Match an identifier, returning NULL on failure, else a pointer to a
   buffer containing the identifier.  */
static char *
match_identifier ()
{
  int lastpos = pos - 1;
  while (isalnum (linebuf[lastpos + 1]) || linebuf[lastpos + 1] == '_')
    if (++lastpos >= LINELEN - 1)
      {
	(*diag) ("line length overrun.\n");
	exit (EC_INTERR);
      }

  if (lastpos < pos)
    return 0;

  char *buf = (char *) malloc (lastpos - pos + 2);
  memcpy (buf, &linebuf[pos], lastpos - pos + 1);
  buf[lastpos - pos + 1] = '\0';

  pos = lastpos + 1;
  return buf;
}

/* Match an integer and return its value, or MININT on failure.  */
static int
match_integer ()
{
  int startpos = pos;
  if (linebuf[pos] == '-')
    safe_inc_pos ();

  int lastpos = pos - 1;
  while (isdigit (linebuf[lastpos + 1]))
    if (++lastpos >= LINELEN - 1)
      {
	(*diag) ("line length overrun in match_integer.\n");
	exit (EC_INTERR);
      }

  if (lastpos < pos)
    return MININT;

  pos = lastpos + 1;
  char *buf = (char *) malloc (lastpos - startpos + 2);
  memcpy (buf, &linebuf[startpos], lastpos - startpos + 1);
  buf[lastpos - startpos + 1] = '\0';

  int x;
  sscanf (buf, "%d", &x);
  return x;
}

static const char *
match_to_right_bracket ()
{
  int lastpos = pos - 1;
  while (linebuf[lastpos + 1] != ']')
    if (++lastpos >= LINELEN - 1)
      {
	(*diag) ("line length overrun.\n");
	exit (EC_INTERR);
      }

  if (lastpos < pos)
    return 0;

  char *buf = (char *) malloc (lastpos - pos + 2);
  memcpy (buf, &linebuf[pos], lastpos - pos + 1);
  buf[lastpos - pos + 1] = '\0';

  pos = lastpos + 1;
  return buf;
}

static inline void
handle_pointer (typeinfo *typedata)
{
  consume_whitespace ();
  if (linebuf[pos] == '*')
    {
      typedata->ispointer = 1;
      safe_inc_pos ();
    }
}

static bif_stanza
stanza_name_to_stanza (const char *stanza_name)
{
  for (int i = 0; i < NUMBIFSTANZAS; i++)
    if (!strcmp (stanza_name, stanza_map[i].stanza_name))
      return stanza_map[i].stanza;
  assert (false);
}

/* Match one of the allowable base types.  Consumes one token unless the
   token is "long", which must be paired with a second "long".  Optionally
   consumes a following '*' token for pointers.  Return 1 for success,
   0 for failure.  */
static int
match_basetype (typeinfo *typedata)
{
  consume_whitespace ();
  int oldpos = pos;
  char *token = match_identifier ();
  if (!token)
    {
      (*diag) ("missing base type in return type at column %d\n", pos + 1);
      return 0;
    }

  if (!strcmp (token, "char"))
    typedata->base = BT_CHAR;
  else if (!strcmp (token, "short"))
    typedata->base = BT_SHORT;
  else if (!strcmp (token, "int"))
    typedata->base = BT_INT;
  else if (!strcmp (token, "long"))
    {
      consume_whitespace ();
      char *mustbelong = match_identifier ();
      if (!mustbelong || strcmp (mustbelong, "long"))
	{
	  (*diag) ("incomplete 'long long' at column %d\n", oldpos + 1);
	  return 0;
	}
      typedata->base = BT_LONGLONG;
    }
  else if (!strcmp (token, "float"))
    typedata->base = BT_FLOAT;
  else if (!strcmp (token, "double"))
    typedata->base = BT_DOUBLE;
  else if (!strcmp (token, "__int128"))
    typedata->base = BT_INT128;
  else if (!strcmp (token, "_Float128"))
    typedata->base = BT_FLOAT128;
  else if (!strcmp (token, "_Decimal32"))
    typedata->base = BT_DECIMAL32;
  else if (!strcmp (token, "_Decimal64"))
    typedata->base = BT_DECIMAL64;
  else if (!strcmp (token, "_Decimal128"))
    typedata->base = BT_DECIMAL128;
  else if (!strcmp (token, "__ibm128"))
    typedata->base = BT_IBM128;
  else
    {
      (*diag) ("unrecognized base type at column %d\n", oldpos + 1);
      return 0;
    }

  handle_pointer (typedata);
  return 1;
}

/* A const int argument may be restricted to certain values.  This is
   indicated by one of the following occurring after the "int' token:

     <x>   restricts the constant to x bits, interpreted as unsigned
     <x,y> restricts the constant to the inclusive range [x,y]
     [x,y] restricts the constant to the inclusive range [x,y],
	   but only applies if the argument is constant.
     {x,y} restricts the constant to one of two values, x or y.

   Here x and y are integer tokens.  Note that the "const" token is a
   lie when the restriction is [x,y], but this simplifies the parsing
   significantly and is hopefully forgivable.

   Return 1 for success, else 0.  */
static int
match_const_restriction (typeinfo *typedata)
{
  int oldpos = pos;
  if (linebuf[pos] == '<')
    {
      safe_inc_pos ();
      oldpos = pos;
      int x = match_integer ();
      if (x == MININT)
	{
	  (*diag) ("malformed integer at column %d.\n", oldpos + 1);
	  return 0;
	}
      consume_whitespace ();
      if (linebuf[pos] == '>')
	{
	  typedata->restr = RES_BITS;
	  typedata->val1 = x;
	  safe_inc_pos ();
	  return 1;
	}
      else if (linebuf[pos] != ',')
	{
	  (*diag) ("malformed restriction at column %d.\n", pos + 1);
	  return 0;
	}
      safe_inc_pos ();
      oldpos = pos;
      int y = match_integer ();
      if (y == MININT)
	{
	  (*diag) ("malformed integer at column %d.\n", oldpos + 1);
	  return 0;
	}
      typedata->restr = RES_RANGE;
      typedata->val1 = x;
      typedata->val2 = y;

      consume_whitespace ();
      if (linebuf[pos] != '>')
	{
	  (*diag) ("malformed restriction at column %d.\n", pos + 1);
	  return 0;
	}
      safe_inc_pos ();
    }
  else if (linebuf[pos] == '{')
    {
      safe_inc_pos ();
      oldpos = pos;
      int x = match_integer ();
      if (x == MININT)
	{
	  (*diag) ("malformed integer at column %d.\n", oldpos + 1);
	  return 0;
	}
      consume_whitespace ();
      if (linebuf[pos] != ',')
	{
	  (*diag) ("missing comma at column %d.\n", pos + 1);
	  return 0;
	}
      safe_inc_pos ();
      consume_whitespace ();
      oldpos = pos;
      int y = match_integer ();
      if (y == MININT)
	{
	  (*diag) ("malformed integer at column %d.\n", oldpos + 1);
	  return 0;
	}
      typedata->restr = RES_VALUES;
      typedata->val1 = x;
      typedata->val2 = y;

      consume_whitespace ();
      if (linebuf[pos] != '}')
	{
	  (*diag) ("malformed restriction at column %d.\n", pos + 1);
	  return 0;
	}
      safe_inc_pos ();
    }
  else
    {
      assert (linebuf[pos] == '[');
      safe_inc_pos ();
      oldpos = pos;
      int x = match_integer ();
      if (x == MININT)
	{
	  (*diag) ("malformed integer at column %d.\n", oldpos + 1);
	  return 0;
	}
      consume_whitespace ();
      if (linebuf[pos] != ',')
	{
	  (*diag) ("missing comma at column %d.\n", pos + 1);
	  return 0;
	}
      safe_inc_pos ();
      consume_whitespace ();
      oldpos = pos;
      int y = match_integer ();
      if (y == MININT)
	{
	  (*diag) ("malformed integer at column %d.\n", oldpos + 1);
	  return 0;
	}
      typedata->restr = RES_VAR_RANGE;
      typedata->val1 = x;
      typedata->val2 = y;

      consume_whitespace ();
      if (linebuf[pos] != ']')
	{
	  (*diag) ("malformed restriction at column %d.\n", pos + 1);
	  return 0;
	}
      safe_inc_pos ();
    }

  return 1;
}

/* Look for a type, which can be terminated by a token that is not part of
   a type, a comma, or a closing parenthesis.  Place information about the
   type in TYPEDATA.  Return 1 for success, 0 for failure.  */
static int
match_type (typeinfo *typedata, int voidok)
{
  /* A legal type is of the form:

       [const] [[signed|unsigned] <basetype> | <vectype>] [*]

     where "const" applies only to a <basetype> of "int".  Legal values
     of <basetype> are (for now):

       char
       short
       int
       long long
       float
       double
       __int128
       _Float128
       _Decimal32
       _Decimal64
       _Decimal128
       __ibm128

     Legal values of <vectype> are as follows, and are shorthand for
     the associated meaning:

       vsc	vector signed char
       vuc	vector unsigned char
       vbc	vector bool char
       vss	vector signed short
       vus	vector unsigned short
       vbs	vector bool short
       vsi	vector signed int
       vui	vector unsigned int
       vbi	vector bool int
       vsll	vector signed long long
       vull	vector unsigned long long
       vbll	vector bool long long
       vsq	vector signed __int128
       vuq	vector unsigned __int128
       vbq	vector bool __int128
       vp	vector pixel
       vf	vector float
       vd	vector double
       vop	opaque vector (matches all vectors)

     For simplicity, We don't support "short int" and "long long int".
     We don't support a <basetype> of "bool", "long double", or "_Float16",
     but will add these if builtins require it.  "signed" and "unsigned"
     only apply to integral base types.  The optional * indicates a pointer
     type, which can be used with any base type, but is treated for type
     signature purposes as a pointer to void.  */

  consume_whitespace ();
  memset (typedata, 0, sizeof(*typedata));
  int oldpos = pos;

  char *token = match_identifier ();
  if (!token)
    return 0;

  if (!strcmp (token, "void"))
    typedata->isvoid = 1;

  if (!strcmp (token, "const"))
    {
      typedata->isconst = 1;
      consume_whitespace ();
      oldpos = pos;
      token = match_identifier ();
    }

  if (!strcmp (token, "vsc"))
    {
      typedata->isvector = 1;
      typedata->issigned = 1;
      typedata->base = BT_CHAR;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vuc"))
    {
      typedata->isvector = 1;
      typedata->isunsigned = 1;
      typedata->base = BT_CHAR;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vbc"))
    {
      typedata->isvector = 1;
      typedata->isbool = 1;
      typedata->base = BT_CHAR;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vss"))
    {
      typedata->isvector = 1;
      typedata->issigned = 1;
      typedata->base = BT_SHORT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vus"))
    {
      typedata->isvector = 1;
      typedata->isunsigned = 1;
      typedata->base = BT_SHORT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vbs"))
    {
      typedata->isvector = 1;
      typedata->isbool = 1;
      typedata->base = BT_SHORT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vsi"))
    {
      typedata->isvector = 1;
      typedata->issigned = 1;
      typedata->base = BT_INT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vui"))
    {
      typedata->isvector = 1;
      typedata->isunsigned = 1;
      typedata->base = BT_INT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vbi"))
    {
      typedata->isvector = 1;
      typedata->isbool = 1;
      typedata->base = BT_INT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vsll"))
    {
      typedata->isvector = 1;
      typedata->issigned = 1;
      typedata->base = BT_LONGLONG;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vull"))
    {
      typedata->isvector = 1;
      typedata->isunsigned = 1;
      typedata->base = BT_LONGLONG;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vbll"))
    {
      typedata->isvector = 1;
      typedata->isbool = 1;
      typedata->base = BT_LONGLONG;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vsq"))
    {
      typedata->isvector = 1;
      typedata->issigned = 1;
      typedata->base = BT_INT128;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vuq"))
    {
      typedata->isvector = 1;
      typedata->isunsigned = 1;
      typedata->base = BT_INT128;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vbq"))
    {
      typedata->isvector = 1;
      typedata->isbool = 1;
      typedata->base = BT_INT128;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vp"))
    {
      typedata->isvector = 1;
      typedata->ispixel = 1;
      typedata->base = BT_SHORT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vf"))
    {
      typedata->isvector = 1;
      typedata->base = BT_FLOAT;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vd"))
    {
      typedata->isvector = 1;
      typedata->base = BT_DOUBLE;
      handle_pointer (typedata);
      return 1;
    }
  else if (!strcmp (token, "vop"))
    {
      typedata->isopaque = 1;
      return 1;
    }
  else if (!strcmp (token, "signed"))
    typedata->issigned = 1;
  else if (!strcmp (token, "unsigned"))
    typedata->isunsigned = 1;
  else if (!typedata->isvoid && !typedata->isconst)
    {
      /* Push back token.  */
      pos = oldpos;
      return match_basetype (typedata);
    }

  if (typedata->isvoid)
    {
      consume_whitespace ();
      if (linebuf[pos] == '*')
	{
	  typedata->ispointer = 1;
	  safe_inc_pos ();
	}
      else if (!voidok)
	return 0;
      return 1;
    }

  if (typedata->isconst)
    {
      consume_whitespace ();
      pos = oldpos;
      token = match_identifier ();
      if (!strcmp (token, "char"))
	{
	  typedata->base = BT_CHAR;
	  handle_pointer (typedata);
	  return 1;
	}
      else if (!strcmp (token, "signed"))
	{
	  typedata->issigned = 1;
	  consume_whitespace ();
	  oldpos = pos;
	  token = match_identifier ();
	  if (strcmp (token, "int"))
	    {
	      (*diag) ("'signed' not followed by 'int' at column %d.\n",
		       oldpos + 1);
	      return 0;
	    }
	}
      else if (!strcmp (token, "unsigned"))
	{
	  typedata->isunsigned = 1;
	  consume_whitespace ();
	  oldpos = pos;
	  token = match_identifier ();
	  if (strcmp (token, "int"))
	    {
	      (*diag) ("'unsigned' not followed by 'int' at column %d.\n",
		       oldpos + 1);
	      return 0;
	    }
	}
      else if (strcmp (token, "int"))
	{
	  (*diag) ("'const' not followed by 'int' at column %d.\n",
		   oldpos + 1);
	  return 0;
	}

      typedata->base = BT_INT;

      consume_whitespace ();
      if (linebuf[pos] == '<' || linebuf[pos] == '{' || linebuf[pos] == '[')
	return match_const_restriction (typedata);

      return 1;
    }

  consume_whitespace ();
  return match_basetype (typedata);
}

/* Parse the argument list.  */
static parse_codes
parse_args (prototype *protoptr)
{
  typelist **argptr = &protoptr->args;
  int *nargs = &protoptr->nargs;
  int *restr_opnd = protoptr->restr_opnd;
  restriction *restr = protoptr->restr;
  int *val1 = protoptr->restr_val1;
  int *val2 = protoptr->restr_val2;
  int restr_cnt = 0;

  int success;
  *nargs = 0;

  /* Start the argument list.  */
  consume_whitespace ();
  if (linebuf[pos] != '(')
    {
      (*diag) ("missing '(' at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  do {
    consume_whitespace ();
    int oldpos = pos;
    typelist *argentry = (typelist *) malloc (sizeof (typelist));
    memset (argentry, 0, sizeof (*argentry));
    typeinfo *argtype = &argentry->info;
    success = match_type (argtype, VOID_NOTOK);
    if (success)
      {
	if (argtype->restr)
	  {
	    if (restr_cnt >= 2)
	      {
		(*diag) ("More than two restricted operands\n");
		return PC_PARSEFAIL;
	      }
	    restr_opnd[restr_cnt] = *nargs + 1;
	    restr[restr_cnt] = argtype->restr;
	    val1[restr_cnt] = argtype->val1;
	    val2[restr_cnt++] = argtype->val2;
	  }
	(*nargs)++;
	*argptr = argentry;
	argptr = &argentry->next;
	consume_whitespace ();
	if (linebuf[pos] == ',')
	  safe_inc_pos ();
	else if (linebuf[pos] != ')')
	  {
	    (*diag) ("arg not followed by ',' or ')' at column %d.\n",
		     pos + 1);
	    return PC_PARSEFAIL;
	  }

#ifdef DEBUG
	(*diag) ("argument type: isvoid = %d, isconst = %d, isvector = %d, \
issigned = %d, isunsigned = %d, isbool = %d, ispixel = %d, ispointer = %d, \
base = %d, restr = %d, val1 = %d, val2 = %d, pos = %d.\n",
		 argtype->isvoid, argtype->isconst, argtype->isvector,
		 argtype->issigned, argtype->isunsigned, argtype->isbool,
		 argtype->ispixel, argtype->ispointer, argtype->base,
		 argtype->restr, argtype->val1, argtype->val2, pos + 1);
#endif
      }
    else
      {
	free (argentry);
	*argptr = NULL;
	pos = oldpos;
	if (linebuf[pos] != ')')
	  {
	    (*diag) ("badly terminated arg list at column %d.\n", pos + 1);
	    return PC_PARSEFAIL;
	  }
	safe_inc_pos ();
      }
  } while (success);

  return PC_OK;
}

/* Parse the attribute list.  */
static parse_codes
parse_bif_attrs (attrinfo *attrptr)
{
  consume_whitespace ();
  if (linebuf[pos] != '{')
    {
      (*diag) ("missing attribute set at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  memset (attrptr, 0, sizeof (*attrptr));
  char *attrname = NULL;

  do {
    consume_whitespace ();
    int oldpos = pos;
    attrname = match_identifier ();
    if (attrname)
      {
	if (!strcmp (attrname, "init"))
	  attrptr->isinit = 1;
	else if (!strcmp (attrname, "set"))
	  attrptr->isset = 1;
	else if (!strcmp (attrname, "extract"))
	  attrptr->isextract = 1;
	else if (!strcmp (attrname, "nosoft"))
	  attrptr->isnosoft = 1;
	else if (!strcmp (attrname, "ldvec"))
	  attrptr->isldvec = 1;
	else if (!strcmp (attrname, "stvec"))
	  attrptr->isstvec = 1;
	else if (!strcmp (attrname, "reve"))
	  attrptr->isreve = 1;
	else if (!strcmp (attrname, "pred"))
	  attrptr->ispred = 1;
	else if (!strcmp (attrname, "htm"))
	  attrptr->ishtm = 1;
	else if (!strcmp (attrname, "htmspr"))
	  attrptr->ishtmspr = 1;
	else if (!strcmp (attrname, "htmcr"))
	  attrptr->ishtmcr = 1;
	else if (!strcmp (attrname, "mma"))
	  attrptr->ismma = 1;
	else if (!strcmp (attrname, "no32bit"))
	  attrptr->isno32bit = 1;
	else if (!strcmp (attrname, "cpu"))
	  attrptr->iscpu = 1;
	else if (!strcmp (attrname, "ldstmask"))
	  attrptr->isldstmask = 1;
	else
	  {
	    (*diag) ("unknown attribute at column %d.\n", oldpos + 1);
	    return PC_PARSEFAIL;
	  }

	consume_whitespace ();
	if (linebuf[pos] == ',')
	  safe_inc_pos ();
	else if (linebuf[pos] != '}')
	  {
	    (*diag) ("arg not followed by ',' or '}' at column %d.\n",
		     pos + 1);
	    return PC_PARSEFAIL;
	  }
      }
    else
      {
	pos = oldpos;
	if (linebuf[pos] != '}')
	  {
	    (*diag) ("badly terminated attr set at column %d.\n", pos + 1);
	    return PC_PARSEFAIL;
	  }
	safe_inc_pos ();
      }
  } while (attrname);

#ifdef DEBUG
  (*diag) ("attribute set: init = %d, set = %d, extract = %d, \
nosoft = %d, ldvec = %d, stvec = %d, reve = %d, pred = %d, htm = %d, \
htmspr = %d, htmcr = %d, mma = %d, no32bit = %d, cpu = %d, ldstmask = %d.\n",
	   attrptr->isinit, attrptr->isset, attrptr->isextract,
	   attrptr->isnosoft, attrptr->isldvec, attrptr->isstvec,
	   attrptr->isreve, attrptr->ispred, attrptr->ishtm, attrptr->ishtmspr,
	   attrptr->ishtmcr, attrptr->ismma, attrptr->isno32bit,
	   attrptr->iscpu, attrptr->isldstmask);
#endif

  return PC_OK;
}

/* Convert a vector type into a mode string.  */
static void
complete_vector_type (typeinfo *typeptr, char *buf, int *bufi)
{
  if (typeptr->isbool)
    buf[(*bufi)++] = 'b';
  buf[(*bufi)++] = 'v';
  if (typeptr->ispixel)
    {
      memcpy (&buf[*bufi], "p8hi", 4);
      *bufi += 4;
    }
  else
    {
      switch (typeptr->base)
	{
	case BT_CHAR:
	  memcpy (&buf[*bufi], "16qi", 4);
	  *bufi += 4;
	  break;
	case BT_SHORT:
	  memcpy (&buf[*bufi], "8hi", 3);
	  *bufi += 3;
	  break;
	case BT_INT:
	  memcpy (&buf[*bufi], "4si", 3);
	  *bufi += 3;
	  break;
	case BT_LONGLONG:
	  memcpy (&buf[*bufi], "2di", 3);
	  *bufi += 3;
	  break;
	case BT_FLOAT:
	  memcpy (&buf[*bufi], "4sf", 3);
	  *bufi += 3;
	  break;
	case BT_DOUBLE:
	  memcpy (&buf[*bufi], "2df", 3);
	  *bufi += 3;
	  break;
	case BT_INT128:
	  memcpy (&buf[*bufi], "1ti", 3);
	  *bufi += 3;
	  break;
	case BT_FLOAT128:
	  memcpy (&buf[*bufi], "1tf", 3);
	  *bufi += 3;
	  break;
	default:
	  (*diag) ("unhandled basetype %d.\n", typeptr->base);
	  exit (EC_INTERR);
	}
    }
}

/* Convert a base type into a mode string.  */
static void
complete_base_type (typeinfo *typeptr, char *buf, int *bufi)
{
  switch (typeptr->base)
    {
    case BT_CHAR:
      memcpy (&buf[*bufi], "qi", 2);
      break;
    case BT_SHORT:
      memcpy (&buf[*bufi], "hi", 2);
      break;
    case BT_INT:
      memcpy (&buf[*bufi], "si", 2);
      break;
    case BT_LONGLONG:
      memcpy (&buf[*bufi], "di", 2);
      break;
    case BT_FLOAT:
      memcpy (&buf[*bufi], "sf", 2);
      break;
    case BT_DOUBLE:
      memcpy (&buf[*bufi], "df", 2);
      break;
    case BT_INT128:
      memcpy (&buf[*bufi], "ti", 2);
      break;
    case BT_FLOAT128:
      memcpy (&buf[*bufi], "tf", 2);
      break;
    case BT_DECIMAL32:
      memcpy (&buf[*bufi], "sd", 2);
      break;
    case BT_DECIMAL64:
      memcpy (&buf[*bufi], "dd", 2);
      break;
    case BT_DECIMAL128:
      memcpy (&buf[*bufi], "td", 2);
      break;
    case BT_IBM128:
      memcpy (&buf[*bufi], "if", 2);
      break;
    default:
      (*diag) ("unhandled basetype %d.\n", typeptr->base);
      exit (EC_INTERR);
    }

  *bufi += 2;
}

/* Build a function type descriptor identifier from the return type
   and argument types described by PROTOPTR, and store it if it does
   not already exist.  Return the identifier.  */
static char *
construct_fntype_id (prototype *protoptr)
{
  /* Determine the maximum space for a function type descriptor id.
     Each type requires at most 8 characters (6 for the mode*, 1 for
     the optional 'u' preceding the mode, and 1 for an underscore
     following the mode).  We also need 5 characters for the string
     "ftype" that separates the return mode from the argument modes.
     The last argument doesn't need a trailing underscore, but we
     count that as the one trailing "ftype" instead.  For the special
     case of zero arguments, we need 8 for the return type and 7
     for "ftype_v".  Finally, we need one character for the
     terminating null.  Thus for a function with N arguments, we
     need at most 8N+14 characters for N>0, otherwise 16.
     ----
       *Worst case is bv16qi for "vector bool char".  */
  int len = protoptr->nargs ? (protoptr->nargs + 1) * 8 + 6 : 16;
  char *buf = (char *) malloc (len);
  int bufi = 0;

  if (protoptr->rettype.ispointer)
    {
      assert (protoptr->rettype.isvoid);
      buf[bufi++] = 'p';
    }
  if (protoptr->rettype.isvoid)
    buf[bufi++] = 'v';
  else
    {
      if (protoptr->rettype.isopaque)
	{
	  memcpy (&buf[bufi], "opaque", 6);
	  bufi += 6;
	}
      else
	{
	  if (protoptr->rettype.isunsigned)
	    buf[bufi++] = 'u';
	  if (protoptr->rettype.isvector)
	    complete_vector_type (&protoptr->rettype, buf, &bufi);
	  else
	    complete_base_type (&protoptr->rettype, buf, &bufi);
	}
    }

  memcpy (&buf[bufi], "_ftype", 6);
  bufi += 6;

  if (!protoptr->nargs)
    {
      memcpy (&buf[bufi], "_v", 2);
      bufi += 2;
    }
  else
    {
      typelist *argptr = protoptr->args;
      for (int i = 0; i < protoptr->nargs; i++)
	{
	  assert (argptr);
	  buf[bufi++] = '_';
	  if (argptr->info.ispointer)
	    {
	      buf[bufi++] = 'p';
	      buf[bufi++] = 'v';
	    }
	  else
	    {
	      if (argptr->info.isopaque)
		{
		  memcpy (&buf[bufi], "opaque", 6);
		  bufi += 6;
		}
	      else
		{
		  if (argptr->info.isunsigned)
		    buf[bufi++] = 'u';
		  if (argptr->info.isvector)
		    complete_vector_type (&argptr->info, buf, &bufi);
		  else
		    complete_base_type (&argptr->info, buf, &bufi);
		}
	    }
	  argptr = argptr->next;
	}
      assert (!argptr);
      }

  buf[bufi] = '\0';

  /* Ignore return value, as duplicates are expected.  */
  (void) rbt_insert (&fntype_rbt, buf);

  return buf;
}

/* Parse a function prototype.  This code is shared by the bif and overload
   file processing.  */
static parse_codes
parse_prototype (prototype *protoptr)
{
  typeinfo *ret_type = &protoptr->rettype;
  char **bifname = &protoptr->bifname;

  /* Get the return type.  */
  consume_whitespace ();
  int oldpos = pos;
  int success = match_type (ret_type, VOID_OK);
  if (!success)
    {
      (*diag) ("missing or badly formed return type at column %d.\n",
	       oldpos + 1);
      return PC_PARSEFAIL;
    }

#ifdef DEBUG
  (*diag) ("return type: isvoid = %d, isconst = %d, isvector = %d, \
issigned = %d, isunsigned = %d, isbool = %d, ispixel = %d, ispointer = %d, \
base = %d, restr[0] = %d, val1[0] = %d, val2[0] = %d, restr1[1] = %d, \
val1[1] = %d, val2[1] = %d, pos = %d.\n",
	   ret_type->isvoid, ret_type->isconst, ret_type->isvector,
	   ret_type->issigned, ret_type->isunsigned, ret_type->isbool,
	   ret_type->ispixel, ret_type->ispointer, ret_type->base,
	   ret_type->restr, ret_type->val1, ret_type->val2, pos + 1);
#endif

  /* Get the bif name.  */
  consume_whitespace ();
  oldpos = pos;
  *bifname = match_identifier ();
  if (!*bifname)
    {
      (*diag) ("missing function name at column %d.\n", oldpos + 1);
      return PC_PARSEFAIL;
    }

#ifdef DEBUG
  (*diag) ("function name is '%s'.\n", *bifname);
#endif

  /* Process arguments.  */
  if (parse_args (protoptr) == PC_PARSEFAIL)
    return PC_PARSEFAIL;

  /* Process terminating semicolon.  */
  consume_whitespace ();
  if (linebuf[pos] != ';')
    {
      (*diag) ("missing semicolon at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();
  consume_whitespace ();
  if (linebuf[pos] != '\n')
    {
      (*diag) ("garbage at end of line at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }

  return PC_OK;
}

/* Parse a two-line entry for a built-in function.  */
static parse_codes
parse_bif_entry ()
{
  /* Check for end of stanza.  */
  pos = 0;
  consume_whitespace ();
  if (linebuf[pos] == '[')
    return PC_EOSTANZA;

  /* Allocate an entry in the bif table.  */
  if (num_bifs >= MAXBIFS - 1)
    {
      (*diag) ("too many built-in functions.\n");
      return PC_PARSEFAIL;
    }

  curr_bif = num_bifs++;
  bifs[curr_bif].stanza = curr_bif_stanza;

  /* Read the first token and see if it is a function modifier.  */
  consume_whitespace ();
  int oldpos = pos;
  char *token = match_identifier ();
  if (!token)
    {
      (*diag) ("malformed entry at column %d\n", pos + 1);
      return PC_PARSEFAIL;
    }

  if (!strcmp (token, "const"))
    bifs[curr_bif].kind = FNK_CONST;
  else if (!strcmp (token, "pure"))
    bifs[curr_bif].kind = FNK_PURE;
  else if (!strcmp (token, "fpmath"))
    bifs[curr_bif].kind = FNK_FPMATH;
  else
    {
      /* No function modifier, so push the token back.  */
      pos = oldpos;
      bifs[curr_bif].kind = FNK_NONE;
    }

  if (parse_prototype (&bifs[curr_bif].proto) == PC_PARSEFAIL)
    return PC_PARSEFAIL;

  /* Build a function type descriptor identifier from the return type
     and argument types, and store it if it does not already exist.  */
  bifs[curr_bif].fndecl = construct_fntype_id (&bifs[curr_bif].proto);

  /* Now process line 2.  First up is the builtin id.  */
  if (!advance_line (bif_file))
    {
      (*diag) ("unexpected EOF.\n");
      return PC_PARSEFAIL;
    }

  pos = 0;
  consume_whitespace ();
  oldpos = pos;
  bifs[curr_bif].idname = match_identifier ();
  if (!bifs[curr_bif].idname)
    {
      (*diag) ("missing builtin id at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }

#ifdef DEBUG
  (*diag) ("ID name is '%s'.\n", bifs[curr_bif].idname);
#endif

  /* Save the ID in a lookup structure.  */
  if (!rbt_insert (&bif_rbt, bifs[curr_bif].idname))
    {
      (*diag) ("duplicate function ID '%s' at column %d.\n",
	       bifs[curr_bif].idname, oldpos + 1);
      return PC_PARSEFAIL;
    }

  /* Now the pattern name.  */
  consume_whitespace ();
  bifs[curr_bif].patname = match_identifier ();
  if (!bifs[curr_bif].patname)
    {
      (*diag) ("missing pattern name at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }

#ifdef DEBUG
  (*diag) ("pattern name is '%s'.\n", bifs[curr_bif].patname);
#endif

  /* Process attributes.  */
  return parse_bif_attrs (&bifs[curr_bif].attrs);
}

/* Parse one stanza of the input BIF file.  linebuf already contains the
   first line to parse.  */
static parse_codes
parse_bif_stanza ()
{
  /* Parse the stanza header.  */
  pos = 0;
  consume_whitespace ();

  if (linebuf[pos] != '[')
    {
      (*diag) ("ill-formed stanza header at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  const char *stanza_name = match_to_right_bracket ();
  if (!stanza_name)
    {
      (*diag) ("no expression found in stanza header.\n");
      return PC_PARSEFAIL;
    }

  curr_bif_stanza = stanza_name_to_stanza (stanza_name);

  if (linebuf[pos] != ']')
    {
      (*diag) ("ill-formed stanza header at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  consume_whitespace ();
  if (linebuf[pos] != '\n' && pos != LINELEN - 1)
    {
      (*diag) ("garbage after stanza header.\n");
      return PC_PARSEFAIL;
    }

  parse_codes result = PC_OK;

  while (result != PC_EOSTANZA)
    {
      if (!advance_line (bif_file))
	return PC_EOFILE;
      result = parse_bif_entry ();
      if (result == PC_PARSEFAIL)
	return PC_PARSEFAIL;
    }

  return PC_OK;
}

/* Parse the built-in file.  */
static parse_codes
parse_bif ()
{
  parse_codes result;
  diag = &bif_diag;
  if (!advance_line (bif_file))
    return PC_OK;

  do
    result = parse_bif_stanza ();
  while (result == PC_OK);

  if (result == PC_EOFILE)
    return PC_OK;
  return result;
}

/* Parse one two-line entry in the overload file.  */
static parse_codes
parse_ovld_entry ()
{
  /* Check for end of stanza.  */
  pos = 0;
  consume_whitespace ();
  if (linebuf[pos] == '[')
    return PC_EOSTANZA;

  /* Allocate an entry in the overload table.  */
  if (num_ovlds >= MAXOVLDS - 1)
    {
      (*diag) ("too many overloads.\n");
      return PC_PARSEFAIL;
    }

  curr_ovld = num_ovlds++;
  ovlds[curr_ovld].stanza = curr_ovld_stanza;

  if (parse_prototype (&ovlds[curr_ovld].proto) == PC_PARSEFAIL)
    return PC_PARSEFAIL;

  /* Build a function type descriptor identifier from the return type
     and argument types, and store it if it does not already exist.  */
  ovlds[curr_ovld].fndecl = construct_fntype_id (&ovlds[curr_ovld].proto);

  /* Now process line 2, which just contains the builtin id.  */
  if (!advance_line (ovld_file))
    {
      (*diag) ("unexpected EOF.\n");
      return PC_EOFILE;
    }

  pos = 0;
  consume_whitespace ();
  int oldpos = pos;
  char *id = match_identifier ();
  ovlds[curr_ovld].idname = id;
  if (!id)
    {
      (*diag) ("missing overload id at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }

#ifdef DEBUG
  (*diag) ("ID name is '%s'.\n", id);
#endif

  /* The builtin id has to match one from the bif file.  */
  if (!rbt_find (&bif_rbt, id))
    {
      (*diag) ("builtin ID '%s' not found in bif file.\n", id);
      return PC_PARSEFAIL;
    }

  /* Save the ID in a lookup structure.  */
  if (!rbt_insert (&ovld_rbt, id))
    {
      (*diag) ("duplicate function ID '%s' at column %d.\n", id, oldpos + 1);
      return PC_PARSEFAIL;
    }

  consume_whitespace ();
  if (linebuf[pos] != '\n')
    {
      (*diag) ("garbage at end of line at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  return PC_OK;
}

/* Parse one stanza of the input overload file.  linebuf already contains the
   first line to parse.  */
static parse_codes
parse_ovld_stanza ()
{
  /* Parse the stanza header.  */
  pos = 0;
  consume_whitespace ();

  if (linebuf[pos] != '[')
    {
      (*diag) ("ill-formed stanza header at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  char *stanza_name = match_identifier ();
  if (!stanza_name)
    {
      (*diag) ("no identifier found in stanza header.\n");
      return PC_PARSEFAIL;
    }

  /* Add the identifier to a table and set the number to be recorded
     with subsequent overload entries.  */
  if (num_ovld_stanzas >= MAXOVLDSTANZAS)
    {
      (*diag) ("too many stanza headers.\n");
      return PC_PARSEFAIL;
    }

  curr_ovld_stanza = num_ovld_stanzas++;
  ovld_stanza *stanza = &ovld_stanzas[curr_ovld_stanza];
  stanza->stanza_id = stanza_name;

  consume_whitespace ();
  if (linebuf[pos] != ',')
    {
      (*diag) ("missing comma at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  consume_whitespace ();
  stanza->extern_name = match_identifier ();
  if (!stanza->extern_name)
    {
      (*diag) ("missing external name at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }

  consume_whitespace ();
  if (linebuf[pos] != ',')
    {
      (*diag) ("missing comma at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  consume_whitespace ();
  stanza->intern_name = match_identifier ();
  if (!stanza->intern_name)
    {
      (*diag) ("missing internal name at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }

  if (linebuf[pos] != ']')
    {
      (*diag) ("ill-formed stanza header at column %d.\n", pos + 1);
      return PC_PARSEFAIL;
    }
  safe_inc_pos ();

  consume_whitespace ();
  if (linebuf[pos] != '\n' && pos != LINELEN - 1)
    {
      (*diag) ("garbage after stanza header.\n");
      return PC_PARSEFAIL;
    }

  parse_codes result = PC_OK;

  while (result != PC_EOSTANZA)
    {
      if (!advance_line (ovld_file))
	return PC_EOFILE;

      result = parse_ovld_entry ();
      if (result == PC_EOFILE || result == PC_PARSEFAIL)
	return result;
    }

  return PC_OK;
}

/* Parse the overload file.  */
static parse_codes
parse_ovld ()
{
  parse_codes result = PC_OK;
  diag = &ovld_diag;
  while (result == PC_OK)
    {
      /* Read ahead one line and check for EOF.  */
      if (!advance_line (ovld_file))
	return PC_OK;

      /* Parse a stanza beginning at this line.  */
      result = parse_ovld_stanza ();
    }
  if (result == PC_EOFILE)
    return PC_OK;
  return result;
}

/* Write a comment at the top of FILE about how the code was generated.  */
static void
write_autogenerated_header (FILE *file)
{
  fprintf (file, "/* Automatically generated by the program '%s'\n",
	   pgm_path);
  fprintf (file, "   from the files '%s' and '%s'.  */\n\n",
	   bif_path, ovld_path);
}

/* Callback functions used in creating enumerations.  */
void write_bif_enum (char *str)
{
  fprintf (header_file, "  RS6000_BIF_%s,\n", str);
}

void write_ovld_enum (char *str)
{
  fprintf (header_file, "  RS6000_OVLD_%s,\n", str);
}

/* Write declarations into the header file.  */
static void
write_decls ()
{
  fprintf (header_file, "enum rs6000_gen_builtins\n{\n  RS6000_BIF_NONE,\n");
  rbt_inorder_callback (&bif_rbt, bif_rbt.rbt_root, write_bif_enum);
  fprintf (header_file, "  RS6000_BIF_MAX\n};\n\n");

  fprintf (header_file, "enum restriction {\n");
  fprintf (header_file, "  RES_NONE,\n");
  fprintf (header_file, "  RES_BITS,\n");
  fprintf (header_file, "  RES_RANGE,\n");
  fprintf (header_file, "  RES_VAR_RANGE,\n");
  fprintf (header_file, "  RES_VALUES\n");
  fprintf (header_file, "};\n\n");

  fprintf (header_file, "enum bif_enable {\n");
  fprintf (header_file, "  ENB_ALWAYS,\n");
  fprintf (header_file, "  ENB_P5,\n");
  fprintf (header_file, "  ENB_P6,\n");
  fprintf (header_file, "  ENB_ALTIVEC,\n");
  fprintf (header_file, "  ENB_VSX,\n");
  fprintf (header_file, "  ENB_P7,\n");
  fprintf (header_file, "  ENB_P7_64,\n");
  fprintf (header_file, "  ENB_P8,\n");
  fprintf (header_file, "  ENB_P8V,\n");
  fprintf (header_file, "  ENB_P9,\n");
  fprintf (header_file, "  ENB_P9_64,\n");
  fprintf (header_file, "  ENB_P9V,\n");
  fprintf (header_file, "  ENB_IEEE128_HW,\n");
  fprintf (header_file, "  ENB_DFP,\n");
  fprintf (header_file, "  ENB_CRYPTO,\n");
  fprintf (header_file, "  ENB_HTM,\n");
  fprintf (header_file, "  ENB_P10,\n");
  fprintf (header_file, "  ENB_MMA\n");
  fprintf (header_file, "};\n\n");

  fprintf (header_file, "struct bifdata\n");
  fprintf (header_file, "{\n");
  fprintf (header_file, "  const char *bifname;\n");
  fprintf (header_file, "  bif_enable enable;\n");
  fprintf (header_file, "  tree fntype;\n");
  fprintf (header_file, "  insn_code icode;\n");
  fprintf (header_file, "  int  nargs;\n");
  fprintf (header_file, "  int  bifattrs;\n");
  fprintf (header_file, "  int  restr_opnd[2];\n");
  fprintf (header_file, "  restriction restr[2];\n");
  fprintf (header_file, "  int  restr_val1[2];\n");
  fprintf (header_file, "  int  restr_val2[2];\n");
  fprintf (header_file, "};\n\n");

  fprintf (header_file, "#define bif_init_bit\t\t(0x00000001)\n");
  fprintf (header_file, "#define bif_set_bit\t\t(0x00000002)\n");
  fprintf (header_file, "#define bif_extract_bit\t\t(0x00000004)\n");
  fprintf (header_file, "#define bif_nosoft_bit\t\t(0x00000008)\n");
  fprintf (header_file, "#define bif_ldvec_bit\t\t(0x00000010)\n");
  fprintf (header_file, "#define bif_stvec_bit\t\t(0x00000020)\n");
  fprintf (header_file, "#define bif_reve_bit\t\t(0x00000040)\n");
  fprintf (header_file, "#define bif_pred_bit\t\t(0x00000080)\n");
  fprintf (header_file, "#define bif_htm_bit\t\t(0x00000100)\n");
  fprintf (header_file, "#define bif_htmspr_bit\t\t(0x00000200)\n");
  fprintf (header_file, "#define bif_htmcr_bit\t\t(0x00000400)\n");
  fprintf (header_file, "#define bif_mma_bit\t\t(0x00000800)\n");
  fprintf (header_file, "#define bif_no32bit_bit\t\t(0x00001000)\n");
  fprintf (header_file, "#define bif_cpu_bit\t\t(0x00002000)\n");
  fprintf (header_file, "#define bif_ldstmask_bit\t(0x00004000)\n");
  fprintf (header_file, "\n");
  fprintf (header_file,
	   "#define bif_is_init(x)\t\t((x).bifattrs & bif_init_bit)\n");
  fprintf (header_file,
	   "#define bif_is_set(x)\t\t((x).bifattrs & bif_set_bit)\n");
  fprintf (header_file,
	   "#define bif_is_extract(x)\t((x).bifattrs & bif_extract_bit)\n");
  fprintf (header_file,
	   "#define bif_is_nosoft(x)\t((x).bifattrs & bif_nosoft_bit)\n");
  fprintf (header_file,
	   "#define bif_is_ldvec(x)\t\t((x).bifattrs & bif_ldvec_bit)\n");
  fprintf (header_file,
	   "#define bif_is_stvec(x)\t\t((x).bifattrs & bif_stvec_bit)\n");
  fprintf (header_file,
	   "#define bif_is_reve(x)\t\t((x).bifattrs & bif_reve_bit)\n");
  fprintf (header_file,
	   "#define bif_is_predicate(x)\t((x).bifattrs & bif_pred_bit)\n");
  fprintf (header_file,
	   "#define bif_is_htm(x)\t\t((x).bifattrs & bif_htm_bit)\n");
  fprintf (header_file,
	   "#define bif_is_htmspr(x)\t((x).bifattrs & bif_htmspr_bit)\n");
  fprintf (header_file,
	   "#define bif_is_htmcr(x)\t\t((x).bifattrs & bif_htmcr_bit)\n");
  fprintf (header_file,
	   "#define bif_is_mma(x)\t\t((x).bifattrs & bif_mma_bit)\n");
  fprintf (header_file,
	   "#define bif_is_no32bit(x)\t((x).bifattrs & bif_no32bit_bit)\n");
  fprintf (header_file,
	   "#define bif_is_cpu(x)\t\t((x).bifattrs & bif_cpu_bit)\n");
  fprintf (header_file,
	   "#define bif_is_ldstmask(x)\t((x).bifattrs "
	   "& bif_ldstmask_bit)\n");
  fprintf (header_file, "\n");

  /* #### Note that the _x is added for now to avoid conflict with
     the existing rs6000_builtin_info[] file while testing.  It will
     be removed as we progress.  */
  fprintf (header_file, "extern bifdata rs6000_builtin_info_x[];\n\n");

  fprintf (header_file,
	   "struct rs6000_bif_hasher : nofree_ptr_hash<bifdata>\n");
  fprintf (header_file, "{\n");
  fprintf (header_file, "  typedef const char *compare_type;\n\n");
  fprintf (header_file, "  static hashval_t hash (bifdata *);\n");
  fprintf (header_file, "  static bool equal (bifdata *, const char *);\n");
  fprintf (header_file, "};\n\n");

  fprintf (header_file, "extern hash_table<rs6000_bif_hasher> bif_hash;\n\n");

  /* Right now we use nonoverlapping numbers for rs6000_gen_builtins
     and rs6000_gen_overloads.  In the old design, these were all in the
     same enum, and I can't yet prove there isn't a dependency on these
     numbers being distinct.  Once this is more clear, I may change
     this to start at zero.  */
  fprintf (header_file, "enum rs6000_gen_overloads\n{\n");
  fprintf (header_file, "  RS6000_OVLD_NONE = RS6000_BIF_MAX + 1,\n");
  rbt_inorder_callback (&ovld_rbt, ovld_rbt.rbt_root, write_ovld_enum);
  fprintf (header_file, "  RS6000_OVLD_MAX\n};\n\n");

  fprintf (header_file, "struct ovlddata\n");
  fprintf (header_file, "{\n");
  fprintf (header_file, "  const char *bifname;\n");
  fprintf (header_file, "  rs6000_gen_builtins bifid;\n");
  fprintf (header_file, "  tree fntype;\n");
  fprintf (header_file, "  ovlddata *next;\n");
  fprintf (header_file, "};\n\n");

  fprintf (header_file, "extern ovlddata rs6000_overload_info[];\n\n");

  fprintf (header_file,
	   "struct rs6000_ovld_hasher : nofree_ptr_hash<ovlddata>\n");
  fprintf (header_file, "{\n");
  fprintf (header_file, "  typedef const char *compare_type;\n\n");
  fprintf (header_file, "  static hashval_t hash (ovlddata *);\n");
  fprintf (header_file, "  static bool equal (ovlddata *, const char *);\n");
  fprintf (header_file, "};\n\n");

  fprintf (header_file,
	   "extern hash_table<rs6000_ovld_hasher> ovld_hash;\n\n");

  fprintf (header_file, "extern void rs6000_autoinit_builtins ();\n\n");
}

/* Callback functions used for generating trees for function types.  */
void
write_extern_fntype (char *str)
{
  fprintf (header_file, "extern tree %s;\n", str);
}

/* Write everything to the header file (rs6000-builtins.h).  */
static int
write_header_file ()
{
  write_autogenerated_header (header_file);
  fprintf (header_file, "#include \"config.h\"\n");
  fprintf (header_file, "#include \"system.h\"\n");
  fprintf (header_file, "#include \"coretypes.h\"\n");
  fprintf (header_file, "#include \"backend.h\"\n");
  fprintf (header_file, "#include \"rtl.h\"\n");
  fprintf (header_file, "#include \"tree.h\"\n");
  fprintf (header_file, "\n");
  fprintf (header_file, "extern int new_builtins_are_live;\n\n");

  write_decls ();

  /* Write function type list declarators to the header file.  */
  rbt_inorder_callback (&fntype_rbt, fntype_rbt.rbt_root, write_extern_fntype);
  fprintf (header_file, "\n");

  return 1;
}

/* Write everything to the initialization file (rs6000-builtins.c).  */
static int
write_init_file ()
{
  return 1;
}

/* Write everything to the include file (rs6000-vecdefines.h).  */
static int
write_defines_file ()
{
  for (int i = 0; i < num_ovld_stanzas; i++)
    fprintf (defines_file, "#define %s %s\n",
	     ovld_stanzas[i].extern_name,
	     ovld_stanzas[i].intern_name);
  return 1;
}

/* Close and delete output files after any failure, so that subsequent
   build dependencies will fail.  */
static void
delete_output_files ()
{
  /* Depending on whence we're called, some of these may already be
     closed.  Don't check for errors.  */
  fclose (header_file);
  fclose (init_file);
  fclose (defines_file);

  unlink (header_path);
  unlink (init_path);
  unlink (defines_path);
}

/* Main program to convert flat files into built-in initialization code.  */
int
main (int argc, const char **argv)
{
  if (argc != 6)
    {
      fprintf (stderr,
	       "Five arguments required: two input file and three output "
	       "files.\n");
      exit (EC_BADARGS);
    }

  pgm_path = argv[0];
  bif_path = argv[1];
  ovld_path = argv[2];
  header_path = argv[3];
  init_path = argv[4];
  defines_path = argv[5];

  bif_file = fopen (bif_path, "r");
  if (!bif_file)
    {
      fprintf (stderr, "Cannot find input built-in file '%s'.\n", bif_path);
      exit (EC_NOBIF);
    }
  ovld_file = fopen (ovld_path, "r");
  if (!ovld_file)
    {
      fprintf (stderr, "Cannot find input overload file '%s'.\n", ovld_path);
      exit (EC_NOOVLD);
    }
  header_file = fopen (header_path, "w");
  if (!header_file)
    {
      fprintf (stderr, "Cannot open header file '%s' for output.\n",
	       header_path);
      exit (EC_NOHDR);
    }
  init_file = fopen (init_path, "w");
  if (!init_file)
    {
      fprintf (stderr, "Cannot open init file '%s' for output.\n", init_path);
      exit (EC_NOINIT);
    }
  defines_file = fopen (defines_path, "w");
  if (!defines_file)
    {
      fprintf (stderr, "Cannot open defines file '%s' for output.\n",
	       defines_path);
      exit (EC_NODEFINES);
    }

  /* Initialize the balanced trees containing built-in function ids,
     overload function ids, and function type declaration ids.  */
  bif_rbt.rbt_nil = (rbt_string_node *) malloc (sizeof (rbt_string_node));
  bif_rbt.rbt_nil->color = RBT_BLACK;
  bif_rbt.rbt_root = bif_rbt.rbt_nil;

  ovld_rbt.rbt_nil = (rbt_string_node *) malloc (sizeof (rbt_string_node));
  ovld_rbt.rbt_nil->color = RBT_BLACK;
  ovld_rbt.rbt_root = ovld_rbt.rbt_nil;

  fntype_rbt.rbt_nil = (rbt_string_node *) malloc (sizeof (rbt_string_node));
  fntype_rbt.rbt_nil->color = RBT_BLACK;
  fntype_rbt.rbt_root = fntype_rbt.rbt_nil;

  /* Parse the built-in function file.  */
  num_bifs = 0;
  line = 0;
  if (parse_bif () == PC_PARSEFAIL)
    {
      fprintf (stderr, "Parsing of '%s' failed, aborting.\n", bif_path);
      delete_output_files ();
      exit (EC_PARSEBIF);
    }
  fclose (bif_file);

#ifdef DEBUG
  fprintf (stderr, "\nFunction ID list:\n");
  rbt_dump (&bif_rbt, bif_rbt.rbt_root);
  fprintf (stderr, "\n");
#endif

  /* Parse the overload file.  */
  num_ovld_stanzas = 0;
  num_ovlds = 0;
  line = 0;
  if (parse_ovld () == PC_PARSEFAIL)
    {
      fprintf (stderr, "Parsing of '%s' failed, aborting.\n", ovld_path);
      delete_output_files ();
      exit (EC_PARSEOVLD);
    }
  fclose (ovld_file);

#ifdef DEBUG
  fprintf (stderr, "\nFunction type decl list:\n");
  rbt_dump (&fntype_rbt, fntype_rbt.rbt_root);
  fprintf (stderr, "\n");
#endif

  /* Write the header file and the file containing initialization code.  */
  if (!write_header_file ())
    {
      fprintf (stderr, "Output to '%s' failed, aborting.\n", header_path);
      delete_output_files ();
      exit (EC_WRITEHDR);
    }
  fclose (header_file);
  if (!write_init_file ())
    {
      fprintf (stderr, "Output to '%s' failed, aborting.\n", init_path);
      delete_output_files ();
      exit (EC_WRITEINIT);
    }
  fclose (init_file);

  /* Write the defines file to be included into altivec.h.  */
  if (!write_defines_file ())
    {
      fprintf (stderr, "Output to '%s' failed, aborting.\n", defines_path);
      delete_output_files ();
      exit (EC_WRITEDEFINES);
    }
  fclose (defines_file);

  return EC_OK;
}

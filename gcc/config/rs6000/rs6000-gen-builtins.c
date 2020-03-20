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
   identifying the option mask for which the group of functions is
   permitted, with the mask in square brackets.  This is the only
   information allowed on the stanza header line, other than
   whitespace.

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
     no32bit  Not valid for TARGET_32BIT
     cpu      This is a "cpu_is" or "cpu_supports" builtin
     ldstmask Altivec mask for load or store

   An example stanza might look like this:

[TARGET_ALTIVEC]
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

static int num_bif_stanzas;

/* Legal base types for an argument or return type.  */
enum basetype {
  BT_CHAR,
  BT_SHORT,
  BT_INT,
  BT_LONGLONG,
  BT_FLOAT,
  BT_DOUBLE,
  BT_INT128,
  BT_FLOAT128
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

static int num_bifs;
static int num_ovld_stanzas;
static int num_ovlds;

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
    typedata->isconst = 1;
  else if (!strcmp (token, "vsc"))
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
  else if (!typedata->isvoid)
    {
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
      oldpos = pos;
      token = match_identifier ();
      if (!strcmp (token, "signed"))
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
      if (linebuf[pos] == '<' || linebuf[pos] == '{')
	return match_const_restriction (typedata);

      return 1;
    }

  consume_whitespace ();
  return match_basetype (typedata);
}

/* Parse the built-in file.  Return 1 for success, 5 for a parsing failure.  */
static int
parse_bif ()
{
  return 1;
}

/* Parse the overload file.  Return 1 for success, 6 for a parsing error.  */
static int
parse_ovld ()
{
  return 1;
}

/* Write everything to the header file (rs6000-builtins.h).  */
static int
write_header_file ()
{
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
	       "Five arguments required: two input file and three output"
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
  num_bif_stanzas = 0;
  num_bifs = 0;
  line = 0;
  if (parse_bif () != 1)
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
  if (parse_ovld () != 1)
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

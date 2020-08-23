/* Subroutines used support the pc-relative linker optimization.
   Copyright (C) 2020 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3, or (at your
   option) any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

/* This file implements a RTL pass that looks for pc-relative loads of the
   address of an external variable using the PCREL_GOT relocation and a single
   load that uses that external address.  If that is found we create the
   PCREL_OPT relocation to possibly convert:

	pld addr_reg,var@pcrel@got(0),1

	<possibly other insns that do not use 'addr_reg' or 'data_reg'>

	lwz data_reg,0(addr_reg)

   into:

	plwz data_reg,var@pcrel(0),1

	<possibly other insns that do not use 'addr_reg' or 'data_reg'>

	nop

   If the variable is not defined in the main program or the code using it is
   not in the main program, the linker put the address in the .got section and
   do:

		.section .got
	.Lvar_got:
		.dword var

		.section .text
		pld addr_reg,.Lvar_got@pcrel(0),1

		<possibly other insns that do not use 'addr_reg' or 'data_reg'>

		lwz data_reg,0(addr_reg)

   We only look for a single usage in the basic block where the external
   address is loaded.  Multiple uses or references in another basic block will
   force us to not use the PCREL_OPT relocation.  */

#define IN_TARGET_CODE 1

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "rtl.h"
#include "tree.h"
#include "memmodel.h"
#include "expmed.h"
#include "optabs.h"
#include "recog.h"
#include "df.h"
#include "tm_p.h"
#include "ira.h"
#include "print-tree.h"
#include "varasm.h"
#include "explow.h"
#include "expr.h"
#include "output.h"
#include "tree-pass.h"
#include "rtx-vector-builder.h"
#include "print-rtl.h"
#include "insn-attr.h"
#include "insn-codes.h"


// Maximum number of insns to scan between the load address and the load that
// uses that address.  This can be bumped up if desired.  If the insns are far
// enough away, the PCREL_OPT optimization probably does not help, since the
// load of the external address has probably completed by the time we do the
// load of the variable at that address.
const int MAX_PCREL_OPT_INSNS	= 10;

/* Next PCREL_OPT label number.  */
static unsigned int pcrel_opt_next_num;

/* Various counters.  */
static struct {
  unsigned long extern_addrs;
  unsigned long loads;
  unsigned long load_separation[MAX_PCREL_OPT_INSNS+1];
} counters;


// Optimize a PC-relative load address to be used in a load.
//
// If the sequence of insns is safe to use the PCREL_OPT optimization (i.e. no
// additional references to the address register, the address register dies at
// the load, and no references to the load), convert insns of the form:
//
//	(set (reg:DI addr)
//	     (symbol_ref:DI "ext_symbol"))
//
//	...
//
//	(set (reg:<MODE> value)
//	     (mem:<MODE> (reg:DI addr)))
//
// into:
//
//	(parallel [(set (reg:DI addr)
//                      (unspec:<MODE> [(symbol_ref:DI "ext_symbol")
//                                      (const_int label_num)
//                                      (const_int 0)]
//                                     UNSPEC_PCREL_OPT_LD_ADDR))
//                 (set (reg:DI data)
//                      (unspec:DI [(const_int 0)]
//	                           UNSPEC_PCREL_OPT_LD_ADDR))])
//
//	...
//
//	(parallel [(set (reg:<MODE>)
//                      (unspec:<MODE> [(mem:<MODE> (reg:DI addr))
//	                                (reg:DI data)
//                                      (const_int label_num)]
//                                     UNSPEC_PCREL_OPT_LD_RELOC))
//                 (clobber (reg:DI addr))])
//
// If the register being loaded is the same register that was used to hold the
// external address, we generate the following insn instead:
//
//	(set (reg:DI data)
//           (unspec:DI [(symbol_ref:DI "ext_symbol")
//                       (const_int label_num)
//                       (const_int 1)]
//	                UNSPEC_PCREL_OPT_LD_ADDR))
//
// In the first insn, we set both the address of the external variable, and
// mark that the variable being loaded both are created in that insn, and are
// consumed in the second insn.  It doesn't matter what mode the register that
// we will ultimately do the load into, so we use DImode.  We just need to mark
// that both registers may be set in the first insn, and will be used in the
// second insn.
//
// The UNSPEC_PCREL_OPT_LD_ADDR insn will generate the load address plus
// a definition of a label (.Lpcrel<n>), while the UNSPEC_PCREL_OPT_LD_RELOC
// insn will generate the .reloc to tell the linker to tie the load address and
// load using that address together.
//
//	pld b,ext_symbol@got@pcrel(0),1
// .Lpcrel1:
//
//	...
//
//	.reloc .Lpcrel1-8,R_PPC64_PCREL_OPT,.-(.Lpcrel1-8)
//	lwz r,0(b)
//
// If ext_symbol is defined in another object file in the main program and we
// are linking the main program, the linker will convert the above instructions
// to:
//
//	plwz r,ext_symbol@got@pcrel(0),1
//
//	...
//
//	nop
//
// Return true if the PCREL_OPT load optimization succeeded.

static bool
do_pcrel_opt_load (rtx_insn *addr_insn,		// insn loading address
		   rtx_insn *load_insn)		// insn using address
{
  rtx addr_set = PATTERN (addr_insn);
  rtx addr_reg = SET_DEST (addr_set);
  rtx addr_symbol = SET_SRC (addr_set);
  rtx load_set = single_set (load_insn);
  rtx reg = SET_DEST (load_set);
  rtx mem = SET_SRC (load_set);
  machine_mode reg_mode = GET_MODE (reg);
  machine_mode mem_mode = GET_MODE (mem);
  rtx mem_inner = mem;
  unsigned int reg_regno = reg_or_subregno (reg);

  // LWA is a DS format instruction, but LWZ is a D format instruction.  We use
  // DImode for the mode to force checking whether the bottom 2 bits are 0.
  // However FPR and vector registers uses the LFIWAX instruction which is
  // indexed only.
  if (GET_CODE (mem) == SIGN_EXTEND && GET_MODE (XEXP (mem, 0)) == SImode)
    {
      if (!INT_REGNO_P (reg_regno))
	return false;

      mem_inner = XEXP (mem, 0);
      mem_mode = DImode;
    }

  else if (GET_CODE (mem) == SIGN_EXTEND
	   || GET_CODE (mem) == ZERO_EXTEND
	   || GET_CODE (mem) == FLOAT_EXTEND)
    {
      mem_inner = XEXP (mem, 0);
      mem_mode = GET_MODE (mem_inner);
    }

  if (!MEM_P (mem_inner))
    return false;

  // If this is LFIWAX or similar instructions that are indexed only, we can't
  // do the optimization.
  enum non_prefixed_form non_prefixed = reg_to_non_prefixed (reg, mem_mode);
  if (non_prefixed == NON_PREFIXED_X)
    return false;

  // The optimization will only work on non-prefixed offsettable loads.
  rtx addr = XEXP (mem_inner, 0);
  enum insn_form iform = address_to_insn_form (addr, mem_mode, non_prefixed);
  if (iform != INSN_FORM_BASE_REG
      && iform != INSN_FORM_D
      && iform != INSN_FORM_DS
      && iform != INSN_FORM_DQ)
    return false;

  // Allocate a new PC-relative label, and update the load external address
  // insn.
  //
  // (parallel [(set (reg load)
  //                 (unspec [(symbol_ref addr_symbol)
  //                          (const_int label_num)
  //                          (const_int 0)]
  //                         UNSPEC_PCREL_OPT_LD_ADDR))
  //            (set (reg addr)
  //                 (unspec [(const_int 0)]
  //	                     UNSPEC_PCREL_OPT_LD_ADDR))])

  ++pcrel_opt_next_num;
  unsigned int addr_regno = reg_or_subregno (addr_reg);
  rtx label_num = GEN_INT (pcrel_opt_next_num);
  rtx reg_di = gen_rtx_REG (DImode, reg_regno);

  PATTERN (addr_insn)
    = ((addr_regno != reg_regno)
       ? gen_pcrel_opt_ld_addr (addr_reg, addr_symbol, label_num, reg_di)
       : gen_pcrel_opt_ld_addr_same_reg (addr_reg, addr_symbol, label_num));

  // Revalidate the insn, backing out of the optimization if the insn is not
  // supported.
  INSN_CODE (addr_insn) = recog (PATTERN (addr_insn), addr_insn, 0);
  if (INSN_CODE (addr_insn) < 0)
    {
      PATTERN (addr_insn) = addr_set;
      INSN_CODE (addr_insn) = recog (PATTERN (addr_insn), addr_insn, 0);
      return false;
    }

  // Update the load insn.  If the mem had a sign/zero/float extend, add that
  // also after doing the UNSPEC.  Add an explicit clobber of the external
  // address register just to make it clear that the address register dies.
  //
  // (parallel [(set (reg:<MODE> data)
  //                 (unspec:<MODE> [(mem (addr_reg)
  //                                 (reg:DI data)
  //                                 (const_int label_num)]
  //                                UNSPEC_PCREL_OPT_LD_RELOC))
  //            (clobber (reg:DI addr_reg))])

  rtvec v_load = gen_rtvec (3, mem_inner, reg_di, label_num);
  rtx new_load = gen_rtx_UNSPEC (GET_MODE (mem_inner), v_load,
				 UNSPEC_PCREL_OPT_LD_RELOC);

  if (GET_CODE (mem) != GET_CODE (mem_inner))
    new_load = gen_rtx_fmt_e (GET_CODE (mem), reg_mode, new_load);

  rtx old_load_set = PATTERN (load_insn);
  rtx new_load_set = gen_rtx_SET (reg, new_load);
  rtx load_clobber = gen_rtx_CLOBBER (VOIDmode,
				      (addr_regno == reg_regno
				       ? gen_rtx_SCRATCH (Pmode)
				       : addr_reg));
  PATTERN (load_insn)
    = gen_rtx_PARALLEL (VOIDmode, gen_rtvec (2, new_load_set, load_clobber));

  // Revalidate the insn, backing out of the optimization if the insn is not
  // supported.

  INSN_CODE (load_insn) = recog (PATTERN (load_insn), load_insn, 0);
  if (INSN_CODE (load_insn) < 0)
    {
      PATTERN (addr_insn) = addr_set;
      INSN_CODE (addr_insn) = recog (PATTERN (addr_insn), addr_insn, 0);

      PATTERN (load_insn) = old_load_set;
      INSN_CODE (load_insn) = recog (PATTERN (load_insn), load_insn, 0);
      return false;
    }

  return true;
}


/* Given an insn, find the next insn in the basic block.  Stop if we find a the
   end of a basic block, such as a label, call or jump, and return NULL.  */

static rtx_insn *
next_active_insn_in_basic_block (rtx_insn *insn)
{
  insn = NEXT_INSN (insn);

  while (insn != NULL_RTX)
    {
      /* If the basic block ends or there is a jump of some kind, exit the
	 loop.  */
      if (CALL_P (insn)
	  || JUMP_P (insn)
	  || JUMP_TABLE_DATA_P (insn)
	  || LABEL_P (insn)
	  || BARRIER_P (insn))
	return NULL;

      /* If this is a real insn, return it.  */
      if (!insn->deleted ()
	  && NONJUMP_INSN_P (insn)
	  && GET_CODE (PATTERN (insn)) != USE
	  && GET_CODE (PATTERN (insn)) != CLOBBER)
	return insn;

      /* Loop for USE, CLOBBER, DEBUG_INSN, NOTEs.  */
      insn = NEXT_INSN (insn);
    }

  return NULL;
}


// Validate that a load is actually a single instruction that can be optimized
// with the PCREL_OPT optimization.

static bool
is_single_instruction (rtx_insn *insn, rtx reg)
{
  if (!REG_P (reg) && !SUBREG_P (reg))
    return false;

  if (get_attr_length (insn) != 4)
    return false;

  // _Decimal128 and IBM extended double are always multiple instructions.
  machine_mode mode = GET_MODE (reg);
  if (mode == TFmode && !TARGET_IEEEQUAD)
    return false;

  if (mode == TDmode || mode == IFmode)
    return false;

  // Don't optimize PLQ/PSTQ instructions
  unsigned int regno = reg_or_subregno (reg);
  unsigned int size = GET_MODE_SIZE (mode);
  if (size >= 16 && !VSX_REGNO_P (regno))
    return false;

  return true;
}


// Given an insn with that loads up a base register with the address of an
// external symbol, see if we can optimize it with the PCREL_OPT optimization.

static void
do_pcrel_opt_addr (rtx_insn *addr_insn)
{
  int num_insns = 0;

  // Do some basic validation.
  rtx addr_set = PATTERN (addr_insn);
  if (GET_CODE (addr_set) != SET)
    return;

  rtx addr_reg = SET_DEST (addr_set);
  rtx addr_symbol = SET_SRC (addr_set);

  if (!base_reg_operand (addr_reg, Pmode)
      || !pcrel_external_address (addr_symbol, Pmode))
    return;

  rtx_insn *insn = addr_insn;
  bool looping = true;
  bool had_load = false;	// whether intermediate insns had a load
  bool had_store = false;	// whether intermediate insns had a store
  bool is_load = false;		// whether the current insn is a load
  bool is_store = false;	// whether the current insn is a store

  // Check the following insns and see if it is a load or store that uses the
  // external address.  If we can't do the optimization, just return.
  while (looping)
    {
      is_load = is_store = false;

      // Don't allow too many insns between the load of the external address
      // and the eventual load or store.
      if (++num_insns >= MAX_PCREL_OPT_INSNS)
	return;

      insn = next_active_insn_in_basic_block (insn);
      if (!insn)
	return;

      // See if the current insn is a load or store
      switch (get_attr_type (insn))
	{
	  // While load of the external address is a 'load' for scheduling
	  // purposes, it should be safe to allow loading other external
	  // addresses between the load of the external address we are
	  // currently looking at and the load or store using that address.
	case TYPE_LOAD:
	  if (get_attr_loads_extern_addr (insn) == LOADS_EXTERN_ADDR_YES)
	    break;
	  /* fall through */

	case TYPE_FPLOAD:
	case TYPE_VECLOAD:
	  is_load = true;
	  break;

	case TYPE_STORE:
	case TYPE_FPSTORE:
	case TYPE_VECSTORE:
	  is_store = true;
	  break;

	  // Don't do the optimization through atomic operations.
	case TYPE_LOAD_L:
	case TYPE_STORE_C:
	case TYPE_HTM:
	case TYPE_HTMSIMPLE:
	  return;

	default:
	  break;
	}

      // If the external addresss register was referenced, it must also die in
      // the same insn.
      if (reg_referenced_p (addr_reg, PATTERN (insn)))
	{
	  if (!dead_or_set_p (insn, addr_reg))
	    return;

	  looping = false;
	}

      // If it dies by being set without being referenced, exit.
      else if (dead_or_set_p (insn, addr_reg))
	return;

      // If it isn't the insn we want, remember if there were loads or stores.
      else
	{
	  had_load |= is_load;
	  had_store |= is_store;
	}
    }

  // If the insn does not use the external address, or the external address
  // register does not die at this insn, we can't do the optimization.
  if (!reg_referenced_p (addr_reg, PATTERN (insn))
      || !dead_or_set_p (insn, addr_reg))
    return;

  rtx set = single_set (insn);
  if (!set)
    return;

  // Optimize loads
  if (is_load)
    {
      // If there were any stores in the insns between loading the external
      // address and doing the load, turn off the optimization.
      if (had_store)
	return;

      rtx reg = SET_DEST (set);
      if (!is_single_instruction (insn, reg))
	return;

      rtx mem = SET_SRC (set);
      switch (GET_CODE (mem))
	{
	case MEM:
	  break;

	case SIGN_EXTEND:
	case ZERO_EXTEND:
	case FLOAT_EXTEND:
	  if (!MEM_P (XEXP (mem, 0)))
	    return;
	  break;

	default:
	  return;
	}

      // If the register being loaded was used or set between the load of
      // the external address and the load using the address, we can't do
      // the optimization.
      if (reg_used_between_p (reg, addr_insn, insn)
	  || reg_set_between_p (reg, addr_insn, insn))
	return;

      // Process the load in detail
      if (do_pcrel_opt_load (addr_insn, insn))
	{
	  counters.loads++;
	  counters.load_separation[num_insns-1]++;
	}
    }

  return;
}


// Optimize pcrel external variable references

static unsigned int
do_pcrel_opt_pass (function *fun)
{
  basic_block bb;
  rtx_insn *insn, *curr_insn = 0;

  memset ((char *) &counters, '\0', sizeof (counters));

  // Dataflow analysis for use-def chains.
  df_set_flags (DF_RD_PRUNE_DEAD_DEFS);
  df_chain_add_problem (DF_DU_CHAIN | DF_UD_CHAIN);
  df_note_add_problem ();
  df_analyze ();
  df_set_flags (DF_DEFER_INSN_RESCAN | DF_LR_RUN_DCE);

  // Look at each basic block to see if there is a load of an external
  // variable's external address, and a single load using that external
  // address.
  FOR_ALL_BB_FN (bb, fun)
    {
      FOR_BB_INSNS_SAFE (bb, insn, curr_insn)
	{
	  if (NONJUMP_INSN_P (insn) && single_set (insn)
	      && get_attr_loads_extern_addr (insn) == LOADS_EXTERN_ADDR_YES)
	    {
	      counters.extern_addrs++;
	      do_pcrel_opt_addr (insn);
	    }
	}
    }

  df_remove_problem (df_chain);
  df_process_deferred_rescans ();
  df_set_flags (DF_RD_PRUNE_DEAD_DEFS | DF_LR_RUN_DCE);
  df_chain_add_problem (DF_UD_CHAIN);
  df_note_add_problem ();
  df_analyze ();

  if (dump_file)
    {
      if (!counters.extern_addrs)
	fprintf (dump_file, "\nNo external symbols were referenced\n");

      else
	{
	  fprintf (dump_file,
		   "\n# of loads of an address of an external symbol = %lu\n",
		   counters.extern_addrs);

	  if (!counters.loads)
	    fprintf (dump_file,
		     "\nNo PCREL_OPT load optimizations were done\n");

	  else
	    {
	      fprintf (dump_file, "# of PCREL_OPT loads = %lu\n",
		       counters.loads);

	      fprintf (dump_file, "# of adjacent PCREL_OPT loads = %lu\n",
		       counters.load_separation[0]);

	      for (int i = 1; i < MAX_PCREL_OPT_INSNS; i++)
		{
		  if (counters.load_separation[i])
		    fprintf (dump_file,
			     "# of PCREL_OPT loads separated by %d insn%s = %lu\n",
			     i, (i == 1) ? "" : "s",
			     counters.load_separation[i]);
		}
	    }
	}

      fprintf (dump_file, "\n");
    }

  return 0;
}


// Optimize pc-relative references for the new PCREL_OPT pass
const pass_data pass_data_pcrel_opt =
{
  RTL_PASS,			// type
  "pcrel_opt",			// name
  OPTGROUP_NONE,		// optinfo_flags
  TV_NONE,			// tv_id
  0,				// properties_required
  0,				// properties_provided
  0,				// properties_destroyed
  0,				// todo_flags_start
  TODO_df_finish,		// todo_flags_finish
};

// Pass data structures
class pcrel_opt : public rtl_opt_pass
{
public:
  pcrel_opt (gcc::context *ctxt)
  : rtl_opt_pass (pass_data_pcrel_opt, ctxt)
  {}

  ~pcrel_opt (void)
  {}

  // opt_pass methods:
  virtual bool gate (function *)
  {
    return (TARGET_PCREL && TARGET_PCREL_OPT && optimize);
  }

  virtual unsigned int execute (function *fun)
  {
    return do_pcrel_opt_pass (fun);
  }

  opt_pass *clone ()
  {
    return new pcrel_opt (m_ctxt);
  }
};

rtl_opt_pass *
make_pass_pcrel_opt (gcc::context *ctxt)
{
  return new pcrel_opt (ctxt);
}

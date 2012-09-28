/* Get previous frame state for an existing frame state.
   Copyright (C) 2012 Red Hat, Inc.
   This file is part of elfutils.

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "cfi.h"
#include <stdlib.h>
#include "libdwflP.h"
#include "../libdw/dwarf.h"

#ifndef MAX
# define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Exact copy from libdwfl/segment.c.  */

static GElf_Addr
segment_start (Dwfl *dwfl, GElf_Addr start)
{
  if (dwfl->segment_align > 1)
    start &= -dwfl->segment_align;
  return start;
}

/* Exact copy from libdwfl/segment.c.  */

static GElf_Addr
segment_end (Dwfl *dwfl, GElf_Addr end)
{
  if (dwfl->segment_align > 1)
    end = (end + dwfl->segment_align - 1) & -dwfl->segment_align;
  return end;
}

static bool
state_get_reg (Dwarf_Frame_State *state, unsigned regno, Dwarf_Addr *val)
{
  if (! dwarf_frame_state_reg_get (state, regno, val))
    {
      __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
      return false;
    }
  return true;
}

static bool
memory_read (Dwarf_Frame_State *state, Dwarf_Addr addr, Dwarf_Addr *result)
{
  if (state->base->pid)
    {
      unsigned long ul;
      if (state->base->regs_bits == 64)
	{
	  bool retval = ebl_memory_read (state->base->ebl, state->base->pid, addr, &ul);
	  *result = ul;
	  return retval;
	}
      /* FIXME: Big endian machines!  */
      /* FIXME: Boundary of a page!  */
      /* FIXME? Unaligned access!  */
      if (! ebl_memory_read (state->base->ebl, state->base->pid, addr, &ul))
	return false;
      *result = ul & 0xffffffff;
      return true;
    }

  if (state->base->core)
    {
      Elf *core = state->base->core;
      Dwfl *dwfl = state->base->dwfl;
      static size_t phnum;
      if (elf_getphdrnum (core, &phnum) < 0)
	{
	  __libdwfl_seterrno (DWFL_E_LIBELF);
	  return false;
	}
      for (size_t cnt = 0; cnt < phnum; ++cnt)
	{
	  GElf_Phdr phdr_mem, *phdr = gelf_getphdr (core, cnt, &phdr_mem);
	  if (phdr == NULL || phdr->p_type != PT_LOAD)
	    continue;
	  /* FIXME */
	  Dwarf_Addr bias = 0;
	  GElf_Addr start = segment_start (dwfl, bias + phdr->p_vaddr);
	  GElf_Addr end = segment_end (dwfl, bias + phdr->p_vaddr + phdr->p_memsz);
	  unsigned bytes = state->base->regs_bits / 8;
	  if (addr < start || addr + bytes > end)
	    continue;
	  Elf_Data *data = elf_getdata_rawchunk (core, phdr->p_offset + addr - start, bytes, ELF_T_ADDR);
	  if (data == NULL)
	    {
	      __libdwfl_seterrno (DWFL_E_LIBELF);
	      return false;
	    }
	  assert (data->d_size == bytes);
	  /* FIXME? Unaligned access!  */
	  if (bytes == 8)
	    *result = *(const uint64_t *) data->d_buf;
	  else
	    *result = *(const uint32_t *) data->d_buf;
	  return true;
	}
      return false;
    }

  abort ();
}

static int
bra_compar (const void *key_voidp, const void *elem_voidp)
{
  Dwarf_Word offset = (uintptr_t) key_voidp;
  const Dwarf_Op *op = elem_voidp;
  return (offset > op->offset) - (offset < op->offset);
}

static bool
expr_eval (Dwarf_Frame_State *state, Dwarf_Frame *frame, const Dwarf_Op *ops, size_t nops, Dwarf_Addr *result)
{
  if (nops == 0)
    {
      __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
      return false;
    }

  /* Prepare CFA first.  It may depend on registers which will be already
     unwound the time we would need to compute it later.  Scan OPS first,
     otherwise we would dead-lock preparing CFA for the CFA computation
     itself.  */
  const Dwarf_Op *ops_scan;
  for (ops_scan = ops; ops_scan < ops + nops; ops_scan++)
    if (ops_scan->atom == DW_OP_call_frame_cfa)
      break;
  Dwarf_Op *cfa_ops;
  size_t cfa_nops;
  Dwarf_Addr cfa;
  bool cfa_valid = (ops_scan < ops + nops
		    && dwarf_frame_cfa (frame, &cfa_ops, &cfa_nops) == 0
		    && expr_eval (state, frame, cfa_ops, cfa_nops, &cfa));

  Dwarf_Addr *stack = NULL;
  size_t stack_used = 0, stack_allocated = 0;
  bool
  push (Dwarf_Addr val)
  {
    if (stack_used == stack_allocated)
      {
	stack_allocated = MAX (stack_allocated * 2, 32);
	Dwarf_Addr *stack_new = realloc (stack, stack_allocated * sizeof (*stack));
	if (stack_new == NULL)
	  {
	    __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
	    return false;
	  }
	stack = stack_new;
      }
    stack[stack_used++] = val;
    return true;
  }
  bool
  pop (Dwarf_Addr *val)
  {
    if (stack_used == 0)
      {
	__libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
	return false;
      }
    *val = stack[--stack_used];
    return true;
  }
  Dwarf_Addr val1, val2;
  bool is_location = false;
  for (const Dwarf_Op *op = ops; op < ops + nops; op++)
    switch (op->atom)
    {
      case DW_OP_breg0 ... DW_OP_breg31:
	if (! state_get_reg (state, op->atom - DW_OP_breg0, &val1))
	  {
	    free (stack);
	    return false;
	  }
	val1 += op->number;
	if (! push (val1))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_bregx:
	if (! state_get_reg (state, op->number, &val1))
	  {
	    free (stack);
	    return false;
	  }
	val1 += op->number2;
	if (! push (val1))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_lit0 ... DW_OP_lit31:
	if (! push (op->atom - DW_OP_lit0))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_plus_uconst:
	if (! pop (&val1) || ! push (val1 + op->number))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_call_frame_cfa:
	if (! cfa_valid || ! push (cfa))
	  {
	    __libdwfl_seterrno (DWFL_E_LIBDW);
	    free (stack);
	    return false;
	  }
	is_location = true;
	break;
      case DW_OP_stack_value:
	is_location = false;
	break;
      case DW_OP_deref:
	if (! pop (&val1) || ! memory_read (state, val1, &val1) || ! push (val1))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_nop:
	break;
      case DW_OP_dup:
	if (! pop (&val1) || ! push (val1) || ! push (val1))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_const1u:
      case DW_OP_const1s:
      case DW_OP_const2u:
      case DW_OP_const2s:
      case DW_OP_const4u:
      case DW_OP_const4s:
      case DW_OP_const8u:
      case DW_OP_const8s:
      case DW_OP_constu:
      case DW_OP_consts:
	if (! push (op->number))
	  {
	    free (stack);
	    return false;
	  }
	break;
      case DW_OP_bra:
	if (! pop (&val1))
	  {
	    free (stack);
	    return false;
	  }
	if (val1 == 0)
	  break;
	/* FALLTHRU */
      case DW_OP_skip:;
	Dwarf_Word offset = op->offset + 1 + 2 + (int16_t) op->number;
	ops_scan = bsearch ((void *) (uintptr_t) offset, ops, nops, sizeof (*ops), bra_compar);
	if (ops_scan == NULL)
	  {
	    free (stack);
	    /* PPC32 vDSO has such invalid operations.  */
	    __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
	    return false;
	  }
	/* Undo the 'for' statement increment.  */
	op = ops_scan - 1;
	break;
      case DW_OP_drop:
	if (! pop (&val1))
	  {
	    free (stack);
	    return false;
	  }
	break;
#define BINOP(atom, op)							\
      case atom:							\
	if (! pop (&val2) || ! pop (&val1) || ! push (val1 op val2))	\
	  {								\
	    free (stack);						\
	    return false;						\
	  }								\
	break;
      BINOP (DW_OP_and, &)
      BINOP (DW_OP_shl, <<)
      BINOP (DW_OP_plus, +)
      BINOP (DW_OP_mul, *)
#undef BINOP
#define BINOP_SIGNED(atom, op)						\
      case atom:							\
	if (! pop (&val2) || ! pop (&val1)				\
	    || ! push ((int64_t) val1 op (int64_t) val2))		\
	  {								\
	    free (stack);						\
	    return false;						\
	  }								\
	break;
      BINOP_SIGNED (DW_OP_le, <=)
      BINOP_SIGNED (DW_OP_ge, >=)
      BINOP_SIGNED (DW_OP_eq, ==)
      BINOP_SIGNED (DW_OP_lt, <)
      BINOP_SIGNED (DW_OP_gt, >)
      BINOP_SIGNED (DW_OP_ne, !=)
#undef BINOP_SIGNED
      default:
	__libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
	return false;
    }
  if (! pop (result))
    {
      free (stack);
      return false;
    }
  free (stack);
  if (is_location && ! memory_read (state, *result, result))
    return false;
  return true;
}

/* Return TRUE and update *STATEP for the unwound frame for successful unwind.
   Return TRUE and set *STATEP to NULL for the outermost frame.  Return FALSE
   (and call __libdwfl_seterrno) otherwise.  */

static bool
have_unwound (Dwarf_Frame_State **statep)
{
  Dwarf_Frame_State *state = *statep, *unwound = state->unwound;
  switch (unwound->pc_state)
  {
    case DWARF_FRAME_STATE_ERROR:
      __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
      *statep = NULL;
      return false;
    case DWARF_FRAME_STATE_PC_SET:
      *statep = unwound;
      return true;
    case DWARF_FRAME_STATE_PC_UNDEFINED:
      *statep = NULL;
      return true;
  }
  abort ();
}

/* Check if PC is in the "_start" function which may have no FDE.
   It corresponds to the GDB get_prev_frame logic "inside entry func".
   Return TRUE if PC is in an outer frame.  Return FALSE (and call
   __libdwfl_seterrno) otherwise.  */

static bool
no_fde (Dwarf_Addr pc, Dwfl_Module *mod, Dwarf_Addr bias)
{
  GElf_Ehdr ehdr_mem, *ehdr = gelf_getehdr (mod->main.elf, &ehdr_mem);
  if (ehdr == NULL)
    {
      __libdwfl_seterrno (DWFL_E_LIBELF);
      return false;
    }
  if (pc < ehdr->e_entry + bias)
    {
      __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
      return false;
    }
  GElf_Sym entry_sym;
  /* "_start" is size-less.  Search for PC, if the closest symbol is the one
     for E_ENTRY it belongs into the function starting at E_ENTRY.  */
  if (dwfl_module_addrsym (mod, pc, &entry_sym, NULL) == NULL
      || entry_sym.st_value != ehdr->e_entry + bias
      || (entry_sym.st_size != 0
	  && pc >= entry_sym.st_value + entry_sym.st_size))
    {
      __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
      return false;
    }
  return true;
}

static bool
handle_cfi (Dwarf_Frame_State **statep, Dwarf_Addr pc, Dwfl_Module *mod, Dwarf_CFI *cfi, Dwarf_Addr bias)
{
  Dwarf_Frame_State *state = *statep;
  Dwarf_Frame *frame;
  if (dwarf_cfi_addrframe (cfi, pc - bias, &frame) != 0)
    {
      int dw_errno = dwarf_errno ();
      if (dw_errno == DWARF_E_NO_MATCH)
	{
	  if (! no_fde (pc, mod, bias))
	    return false;
	  *statep = NULL;
	  return true;
	}
      __libdw_seterrno (dw_errno);
      __libdwfl_seterrno (DWFL_E_LIBDW);
      return false;
    }
  Dwarf_Frame_State *unwound = malloc (sizeof (*unwound) + sizeof (*unwound->regs) * state->base->nregs);
  state->unwound = unwound;
  unwound->base = state->base;
  unwound->unwound = NULL;
  unwound->pc_state = DWARF_FRAME_STATE_ERROR;
  memset (unwound->regs_set, 0, sizeof (unwound->regs_set));
  unwound->signal_frame = frame->fde->cie->signal_frame;
  for (unsigned regno = 0; regno < unwound->base->nregs; regno++)
    {
      Dwarf_Op reg_ops_mem[3], *reg_ops;
      size_t reg_nops;
      if (dwarf_frame_register (frame, regno, reg_ops_mem, &reg_ops, &reg_nops) != 0)
	{
	  __libdwfl_seterrno (DWFL_E_LIBDW);
	  return false;
	}
      Dwarf_Addr regval;
      if (reg_nops == 0)
	{
	  if (reg_ops == reg_ops_mem)
	    {
	      /* REGNO is undefined.  */
	      continue;
	    }
	  else if (reg_ops == NULL)
	    {
	      /* REGNO is same-value.  */
	      if (! state_get_reg (state, regno, &regval))
		return false;
	    }
	  else
	    {
	      __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
	      return false;
	    }
	}
      else if (! expr_eval (state, frame, reg_ops, reg_nops, &regval))
	{
	  /* PPC32 vDSO has various invalid operations, ignore them.  The
	     register will look as unset causing an error later, if used.
	     But PPC32 does not use such registers.  */
	  continue;
	}
      if (! dwarf_frame_state_reg_set (unwound, regno, regval))
	{
	  __libdwfl_seterrno (DWFL_E_UNKNOWN_ERROR);
	  return false;
	}
    }
  if (dwarf_frame_state_reg_get (unwound, frame->fde->cie->return_address_register, &unwound->pc)
      /* X86* has to use DW_CFA_undefined, PPC32 explicitly unwinds it to 0,
         DEFAULT_SAME_VALUE is used to differentiate the archs.  */
      && (frame->cache->default_same_value || unwound->pc != 0))
    unwound->pc_state = DWARF_FRAME_STATE_PC_SET;
  else
    unwound->pc_state = DWARF_FRAME_STATE_PC_UNDEFINED;
  return have_unwound (statep);
}

bool
dwfl_frame_unwind (Dwarf_Frame_State **statep)
{
  Dwarf_Frame_State *state = *statep;
  if (state->unwound)
    return have_unwound (statep);
  Dwarf_Addr pc;
  if (! dwfl_frame_state_pc (state, &pc, NULL))
    return false;
  /* Do not ask for MINUSONE dwfl_frame_state_pc, it would try to unwind STATE
     which would deadlock us.  */
  if (state != state->base->unwound && ! state->signal_frame)
    pc--;
  Dwfl_Module *mod = dwfl_addrmodule (state->base->dwfl, pc);
  if (mod == NULL)
    {
      __libdwfl_seterrno (DWFL_E_NO_DWARF);
      return false;
    }
  Dwarf_Addr bias;
  Dwarf_CFI *cfi_eh = dwfl_module_eh_cfi (mod, &bias);
  if (cfi_eh)
    {
      if (handle_cfi (statep, pc, mod, cfi_eh, bias))
	return true;
      if (state->unwound)
	{
	  assert (state->unwound->pc_state == DWARF_FRAME_STATE_ERROR);
	  return false;
	}
    }
  Dwarf_CFI *cfi_dwarf = dwfl_module_dwarf_cfi (mod, &bias);
  if (cfi_dwarf)
    {
      if (handle_cfi (statep, pc, mod, cfi_dwarf, bias) && state->unwound)
	return true;
      if (state->unwound)
	{
	  assert (state->unwound->pc_state == DWARF_FRAME_STATE_ERROR);
	  return false;
	}
    }
  __libdwfl_seterrno (DWFL_E_NO_DWARF);
  return false;
}
INTDEF(dwfl_frame_unwind)

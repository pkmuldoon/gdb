/* BFD back-end for Hitachi H8/300 COFF binaries.
   Copyright 1990, 91, 92, 93, 94, 95, 1996 Free Software Foundation, Inc.
   Written by Steve Chamberlain, <sac@cygnus.com>.

This file is part of BFD, the Binary File Descriptor library.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "bfd.h"
#include "sysdep.h"
#include "obstack.h"
#include "libbfd.h"
#include "bfdlink.h"
#include "genlink.h"
#include "coff/h8300.h"
#include "coff/internal.h"
#include "libcoff.h"

#define COFF_DEFAULT_SECTION_ALIGNMENT_POWER (1)

/* We derive a hash table from the basic BFD hash table to
   hold entries in the function vector.  Aside from the 
   info stored by the basic hash table, we need the offset
   of a particular entry within the hash table as well as
   the offset where we'll add the next entry.  */

struct funcvec_hash_entry
{
  /* The basic hash table entry.  */
  struct bfd_hash_entry root;

  /* The offset within the vectors section where
     this entry lives.  */
  bfd_vma offset;
};

struct funcvec_hash_table
{
  /* The basic hash table.  */
  struct bfd_hash_table root;

  bfd *abfd;

  /* Offset at which we'll add the next entry.  */
  unsigned int offset;
};

static struct bfd_hash_entry *
funcvec_hash_newfunc
  PARAMS ((struct bfd_hash_entry *, struct bfd_hash_table *, const char *));

static boolean
funcvec_hash_table_init
  PARAMS ((struct funcvec_hash_table *, bfd *,
           struct bfd_hash_entry *(*) PARAMS ((struct bfd_hash_entry *,
                                               struct bfd_hash_table *,
                                               const char *))));

/* To lookup a value in the function vector hash table.  */
#define funcvec_hash_lookup(table, string, create, copy) \
  ((struct funcvec_hash_entry *) \
   bfd_hash_lookup (&(table)->root, (string), (create), (copy)))

/* The derived h8300 COFF linker table.  Note it's derived from
   the generic linker hash table, not the COFF backend linker hash
   table!  We use this to attach additional data structures we
   need while linking on the h8300.  */
struct h8300_coff_link_hash_table
{
  /* The main hash table.  */
  struct generic_link_hash_table root;

  /* Section for the vectors table.  This gets attached to a
     random input bfd, we keep it here for easy access.  */
  asection *vectors_sec;

  /* Hash table of the functions we need to enter into the function
     vector.  */
  struct funcvec_hash_table *funcvec_hash_table;
};

static struct bfd_link_hash_table *h8300_coff_link_hash_table_create
  PARAMS ((bfd *));

/* Get the H8/300 COFF linker hash table from a link_info structure.  */

#define h8300_coff_hash_table(p) \
  ((struct h8300_coff_link_hash_table *) ((coff_hash_table (p))))

/* Initialize fields within a funcvec hash table entry.  Called whenever
   a new entry is added to the funcvec hash table.  */

static struct bfd_hash_entry *
funcvec_hash_newfunc (entry, gen_table, string)
     struct bfd_hash_entry *entry;
     struct bfd_hash_table *gen_table;
     const char *string;
{
  struct funcvec_hash_entry *ret;
  struct funcvec_hash_table *table;

  ret = (struct funcvec_hash_entry *) entry;
  table = (struct funcvec_hash_table *) gen_table;

  /* Allocate the structure if it has not already been allocated by a
     subclass.  */
  if (ret == NULL)
    ret = ((struct funcvec_hash_entry *)
           bfd_hash_allocate (gen_table,
                              sizeof (struct funcvec_hash_entry)));
  if (ret == NULL)
    return NULL;

  /* Call the allocation method of the superclass.  */
  ret = ((struct funcvec_hash_entry *)
         bfd_hash_newfunc ((struct bfd_hash_entry *) ret, gen_table, string));

  if (ret == NULL)
    return NULL;

  /* Note where this entry will reside in the function vector table.  */
  ret->offset = table->offset;

  /* Bump the offset at which we store entries in the function
     vector.  We'd like to bump up the size of the vectors section,
     but it's not easily available here.  */
  if (bfd_get_mach (table->abfd) == bfd_mach_h8300)
    table->offset += 2;
  else if (bfd_get_mach (table->abfd) == bfd_mach_h8300h)
    table->offset += 4;
  else
    return NULL;

  /* Everything went OK.  */
  return (struct bfd_hash_entry *) ret;
}

/* Initialize the function vector hash table.  */

static boolean
funcvec_hash_table_init (table, abfd, newfunc)
     struct funcvec_hash_table *table;
     bfd *abfd;
     struct bfd_hash_entry *(*newfunc) PARAMS ((struct bfd_hash_entry *,
                                                struct bfd_hash_table *,
                                                const char *));
{
  /* Initialize our local fields, then call the generic initialization
     routine.  */
  table->offset = 0;
  table->abfd = abfd;
  return (bfd_hash_table_init (&table->root, newfunc));
}

/* Create the derived linker hash table.  We use a derived hash table
   basically to hold "static" information during an h8/300 coff link
   without using static variables.  */

static struct bfd_link_hash_table *
h8300_coff_link_hash_table_create (abfd)
     bfd *abfd;
{
  struct h8300_coff_link_hash_table *ret;
  ret = ((struct h8300_coff_link_hash_table *)
         bfd_alloc (abfd, sizeof (struct h8300_coff_link_hash_table)));
  if (ret == NULL)
    return NULL;
  if (!_bfd_link_hash_table_init (&ret->root.root, abfd, _bfd_generic_link_hash_newfunc))
    {
      bfd_release (abfd, ret);
      return NULL;
    }

  /* Initialize our data.  */
  ret->vectors_sec = NULL;
  ret->funcvec_hash_table = NULL;

  /* OK.  Everything's intialized, return the base pointer.  */
  return &ret->root.root;
}

/* special handling for H8/300 relocs.
   We only come here for pcrel stuff and return normally if not an -r link.
   When doing -r, we can't do any arithmetic for the pcrel stuff, because
   the code in reloc.c assumes that we can manipulate the targets of
   the pcrel branches.  This isn't so, since the H8/300 can do relaxing, 
   which means that the gap after the instruction may not be enough to
   contain the offset required for the branch, so we have to use the only
   the addend until the final link */

static bfd_reloc_status_type
special (abfd, reloc_entry, symbol, data, input_section, output_bfd,
		 error_message)
     bfd *abfd;
     arelent *reloc_entry;
     asymbol *symbol;
     PTR data;
     asection *input_section;
     bfd *output_bfd;
     char **error_message;
{
  if (output_bfd == (bfd *) NULL)
    return bfd_reloc_continue;

  return bfd_reloc_ok;
}

static reloc_howto_type howto_table[] =
{
  HOWTO (R_RELBYTE, 0, 0, 8, false, 0, complain_overflow_bitfield, special, "8", false, 0x000000ff, 0x000000ff, false),
  HOWTO (R_RELWORD, 0, 1, 16, false, 0, complain_overflow_bitfield, special, "16", false, 0x0000ffff, 0x0000ffff, false),
  HOWTO (R_RELLONG, 0, 2, 32, false, 0, complain_overflow_bitfield, special, "32", false, 0xffffffff, 0xffffffff, false),
  HOWTO (R_PCRBYTE, 0, 0, 8, true, 0, complain_overflow_signed, special, "DISP8", false, 0x000000ff, 0x000000ff, true),
  HOWTO (R_PCRWORD, 0, 1, 16, true, 0, complain_overflow_signed, special, "DISP16", false, 0x0000ffff, 0x0000ffff, true),
  HOWTO (R_PCRLONG, 0, 2, 32, true, 0, complain_overflow_signed, special, "DISP32", false, 0xffffffff, 0xffffffff, true),
  HOWTO (R_MOVB1, 0, 1, 16, false, 0, complain_overflow_bitfield, special, "16/8", false, 0x0000ffff, 0x0000ffff, false),
  HOWTO (R_MOVB2, 0, 1, 16, false, 0, complain_overflow_bitfield, special, "8/16", false, 0x0000ffff, 0x0000ffff, false),
  HOWTO (R_JMP1, 0, 1, 16, false, 0, complain_overflow_bitfield, special, "16/pcrel", false, 0x0000ffff, 0x0000ffff, false),
  HOWTO (R_JMP2, 0, 0, 8, false, 0, complain_overflow_bitfield, special, "pcrecl/16", false, 0x000000ff, 0x000000ff, false),


  HOWTO (R_JMPL1, 0, 2, 32, false, 0, complain_overflow_bitfield, special, "24/pcrell", false, 0x00ffffff, 0x00ffffff, false),
  HOWTO (R_JMPL_B8, 0, 0, 8, false, 0, complain_overflow_bitfield, special, "pc8/24", false, 0x000000ff, 0x000000ff, false),

  HOWTO (R_MOVLB1, 0, 1, 16, false, 0, complain_overflow_bitfield,special, "24/8", false, 0x0000ffff, 0x0000ffff, false),
  HOWTO (R_MOVLB2, 0, 1, 16, false, 0, complain_overflow_bitfield, special, "8/24", false, 0x0000ffff, 0x0000ffff, false),

  /* An indirect reference to a function.  This causes the function's address
     to be added to the function vector in lo-mem and puts the address of
     the function vector's entry in the jsr instruction.  */
  HOWTO (R_MEM_INDIRECT, 0, 0, 8, false, 0, complain_overflow_bitfield, special, "8/indirect", false, 0x000000ff, 0x000000ff, false),

  /* Internal reloc for relaxing.  This is created when a 16bit pc-relative
     branch is turned into an 8bit pc-relative branch.  */
  HOWTO (R_PCRWORD_B, 0, 0, 8, true, 0, complain_overflow_bitfield, special, "pcrecl/16", false, 0x000000ff, 0x000000ff, false),
};


/* Turn a howto into a reloc number */

#define SELECT_RELOC(x,howto) \
  { x.r_type = select_reloc(howto); }

#define BADMAG(x) (H8300BADMAG(x)&& H8300HBADMAG(x))
#define H8300 1			/* Customize coffcode.h */
#define __A_MAGIC_SET__



/* Code to swap in the reloc */
#define SWAP_IN_RELOC_OFFSET   bfd_h_get_32
#define SWAP_OUT_RELOC_OFFSET bfd_h_put_32
#define SWAP_OUT_RELOC_EXTRA(abfd, src, dst) \
  dst->r_stuff[0] = 'S'; \
  dst->r_stuff[1] = 'C';


static int
select_reloc (howto)
     reloc_howto_type *howto;
{
  return howto->type;
}

/* Code to turn a r_type into a howto ptr, uses the above howto table
   */

static void
rtype2howto (internal, dst)
     arelent *internal;
     struct internal_reloc *dst;
{
  switch (dst->r_type)
    {
    case R_RELBYTE:
      internal->howto = howto_table + 0;
      break;
    case R_RELWORD:
      internal->howto = howto_table + 1;
      break;
    case R_RELLONG:
      internal->howto = howto_table + 2;
      break;
    case R_PCRBYTE:
      internal->howto = howto_table + 3;
      break;
    case R_PCRWORD:
      internal->howto = howto_table + 4;
      break;
    case R_PCRLONG:
      internal->howto = howto_table + 5;
      break;
    case R_MOVB1:
      internal->howto = howto_table + 6;
      break;
    case R_MOVB2:
      internal->howto = howto_table + 7;
      break;
    case R_JMP1:
      internal->howto = howto_table + 8;
      break;
    case R_JMP2:
      internal->howto = howto_table + 9;
      break;
    case R_JMPL1:
      internal->howto = howto_table + 10;
      break;
    case R_JMPL_B8:
      internal->howto = howto_table + 11;
      break;
    case R_MOVLB1:
      internal->howto = howto_table + 12;
      break;
    case R_MOVLB2:
      internal->howto = howto_table + 13;
      break;
    case R_MEM_INDIRECT:
      internal->howto = howto_table + 14;
      break;
    case R_PCRWORD_B:
      internal->howto = howto_table + 15;
      break;
    default:
      abort ();
      break;
    }
}

#define RTYPE2HOWTO(internal, relocentry) rtype2howto(internal,relocentry)


/* Perform any necessaru magic to the addend in a reloc entry */


#define CALC_ADDEND(abfd, symbol, ext_reloc, cache_ptr) \
 cache_ptr->addend =  ext_reloc.r_offset;


#define RELOC_PROCESSING(relent,reloc,symbols,abfd,section) \
 reloc_processing(relent, reloc, symbols, abfd, section)

static void
reloc_processing (relent, reloc, symbols, abfd, section)
     arelent * relent;
     struct internal_reloc *reloc;
     asymbol ** symbols;
     bfd * abfd;
     asection * section;
{
  relent->address = reloc->r_vaddr;
  rtype2howto (relent, reloc);

  if (((int) reloc->r_symndx) > 0)
    {
      relent->sym_ptr_ptr = symbols + obj_convert (abfd)[reloc->r_symndx];
    }
  else
    {
      relent->sym_ptr_ptr = bfd_abs_section_ptr->symbol_ptr_ptr;
    }



  relent->addend = reloc->r_offset;

  relent->address -= section->vma;
  /*  relent->section = 0;*/
}


static int
h8300_reloc16_estimate(abfd, input_section, reloc, shrink, link_info)
     bfd *abfd;
     asection *input_section;
     arelent *reloc;
     unsigned int shrink;
     struct bfd_link_info *link_info;
{
  bfd_vma value;  
  bfd_vma dot;
  bfd_vma gap;

  /* The address of the thing to be relocated will have moved back by 
   the size of the shrink  - but we don't change reloc->address here,
   since we need it to know where the relocation lives in the source
   uncooked section */

  /*  reloc->address -= shrink;   conceptual */

  bfd_vma address = reloc->address - shrink;
  

  switch (reloc->howto->type)
    {     
    case R_MOVB2:
    case R_JMP2:
    case R_PCRWORD_B:
      shrink+=2;
      break;

      /* Thing is a move one byte */
    case R_MOVB1:
      value = bfd_coff_reloc16_get_value(reloc, link_info, input_section);

      if (value >= 0xff00)
	{ 

	  /* Change the reloc type from 16bit, possible 8 to 8bit
	     possible 16 */
	  reloc->howto = reloc->howto + 1;	  
	  /* The place to relc moves back by one */
	  /* This will be two bytes smaller in the long run */
	  shrink +=2 ;
	  bfd_perform_slip(abfd, 2, input_section, address);
	}      

      break;
      /* This is the 24 bit branch which could become an 8 bitter, 
       the relocation points to the first byte of the insn, not the
       actual data */

    case R_JMPL1:
      value = bfd_coff_reloc16_get_value(reloc, link_info, input_section);
	
      dot = input_section->output_section->vma +
	input_section->output_offset + address;
  
      /* See if the address we're looking at within 127 bytes of where
	 we are, if so then we can use a small branch rather than the
	 jump we were going to */

      gap = value - dot ;
  
      if (-120 < (long)gap && (long)gap < 120 )
	{ 

	  /* Change the reloc type from 24bit, possible 8 to 8bit
	     possible 32 */
	  reloc->howto = reloc->howto + 1;	  
	  /* This will be two bytes smaller in the long run */
	  shrink +=2 ;
	  bfd_perform_slip(abfd, 2, input_section, address);
	}
      break;

    case R_JMP1:

      value = bfd_coff_reloc16_get_value(reloc, link_info, input_section);
	
      dot = input_section->output_section->vma +
	input_section->output_offset + address;
  
      /* See if the address we're looking at within 127 bytes of where
	 we are, if so then we can use a small branch rather than the
	 jump we were going to */

      gap = value - (dot - shrink);
  

      if (-120 < (long)gap && (long)gap < 120 )
	{ 

	  /* Change the reloc type from 16bit, possible 8 to 8bit
	     possible 16 */
	  reloc->howto = reloc->howto + 1;	  
	  /* The place to relc moves back by one */

	  /* This will be two bytes smaller in the long run */
	  shrink +=2 ;
	  bfd_perform_slip(abfd, 2, input_section, address);
	}
      break;

    case R_PCRWORD:

      value = bfd_coff_reloc16_get_value(reloc, link_info, input_section);
	
      dot = input_section->output_section->vma +
	input_section->output_offset + address - 2;
  
      /* See if the address we're looking at within 127 bytes of where
	 we are, if so then we can use a small branch rather than the
	 jump we were going to */

      gap = value - (dot - shrink);
  

      if (-120 < (long)gap && (long)gap < 120 )
	{ 

	  /* Change the reloc type from 16bit, possible 8 to 8bit
	     possible 16 */
	  reloc->howto = howto_table + 15;
	  /* The place to relc moves back by one */

	  /* This will be two bytes smaller in the long run */
	  shrink +=2 ;
	  bfd_perform_slip(abfd, 2, input_section, address);
	}
      break;
    }

  return shrink;
}


/* First phase of a relaxing link */

/* Reloc types
   large		small
   R_MOVB1		R_MOVB2		mov.b with 16bit or 8 bit address
   R_JMP1		R_JMP2		jmp or pcrel branch
   R_JMPL1		R_JMPL_B8	24jmp or pcrel branch
   R_MOVLB1		R_MOVLB2	24 or 8 bit reloc for mov.b
   R_PCRWORD		R_PCRWORD_B	8 bit pcrel branch from 16bit pcrel
					branch.

*/


static void
h8300_reloc16_extra_cases (abfd, link_info, link_order, reloc, data, src_ptr,
			   dst_ptr)
     bfd *abfd;
     struct bfd_link_info *link_info;
     struct bfd_link_order *link_order;
     arelent *reloc;
     bfd_byte *data;
     unsigned int *src_ptr;
     unsigned int *dst_ptr;
{
  unsigned int src_address = *src_ptr;
  unsigned int dst_address = *dst_ptr;
  asection *input_section = link_order->u.indirect.section;

  switch (reloc->howto->type)
    {
      /* A 24 bit branch which could be a 8 bit pcrel, really pointing to
	 the byte before the 24bit hole, so we can treat it as a 32bit pointer */
    case R_PCRBYTE:
      {
	bfd_vma dot = link_order->offset 
	  + dst_address 
	    + link_order->u.indirect.section->output_section->vma;
	int gap = (bfd_coff_reloc16_get_value (reloc, link_info, input_section)
		   - dot);
	if (gap > 127 || gap < -128)
	  {
	    if (! ((*link_info->callbacks->reloc_overflow)
		   (link_info, bfd_asymbol_name (*reloc->sym_ptr_ptr),
		    reloc->howto->name, reloc->addend, input_section->owner,
		    input_section, reloc->address)))
	      abort ();
	  }
	gap &= ~1;
	bfd_put_8 (abfd, gap, data + dst_address);
	dst_address++;
	src_address++;

	break;
      }
    case R_PCRWORD:
      {
	bfd_vma dot = link_order->offset 
	  + dst_address 
	    + link_order->u.indirect.section->output_section->vma;
	int gap = (bfd_coff_reloc16_get_value (reloc, link_info, input_section)
		   - dot) - 1;
	if (gap > 32767 || gap < -32768)
	  {
	    if (! ((*link_info->callbacks->reloc_overflow)
		   (link_info, bfd_asymbol_name (*reloc->sym_ptr_ptr),
		    reloc->howto->name, reloc->addend, input_section->owner,
		    input_section, reloc->address)))
	      abort ();
	  }

	bfd_put_16 (abfd, gap, data + dst_address);
	dst_address+=2;
	src_address+=2;

	break;
      }

    case R_RELBYTE:
      {
	unsigned int gap = bfd_coff_reloc16_get_value (reloc, link_info,
						       input_section);
	if (gap < 0xff 
	    || (gap >= 0x0000ff00
	        && gap <= 0x0000ffff)
  	    || (   gap >= 0x00ffff00 
		&& gap <= 0x00ffffff)
	    || (   gap >= 0xffffff00
		&& gap <= 0xffffffff))
	  {
	    bfd_put_8 (abfd, gap, data + dst_address);
	    dst_address += 1;
	    src_address += 1;
	  }
	else
	  {
	    if (! ((*link_info->callbacks->reloc_overflow)
		   (link_info, bfd_asymbol_name (*reloc->sym_ptr_ptr),
		    reloc->howto->name, reloc->addend, input_section->owner,
		    input_section, reloc->address)))
	      abort ();
	  }
      }
      break;
    case R_JMP1:
      /* A relword which would have like to have been a pcrel */
    case R_MOVB1:
      /* A relword which would like to have been modified but
	     didn't make it */
    case R_RELWORD:
      bfd_put_16 (abfd,
		  bfd_coff_reloc16_get_value (reloc, link_info, input_section),
		  data + dst_address);
      dst_address += 2;
      src_address += 2;
      break;
    case R_RELLONG:
      bfd_put_32 (abfd,
		  bfd_coff_reloc16_get_value (reloc, link_info, input_section),
		  data + dst_address);
      dst_address += 4;
      src_address += 4;
      break;

    case R_MOVB2:
      /* Special relaxed type, there will be a gap between where we
	     get stuff from and where we put stuff to now
	
	     for a mov.b @aa:16 -> mov.b @aa:8
	     opcode 0x6a 0x0y offset
	     ->     0x2y off
	     */
      if (data[dst_address - 1] != 0x6a)
	abort ();
      switch (data[src_address] & 0xf0)
	{
	case 0x00:
	  /* Src is memory */
	  data[dst_address - 1] = (data[src_address] & 0xf) | 0x20;
	  break;
	case 0x80:
	  /* Src is reg */
	  data[dst_address - 1] = (data[src_address] & 0xf) | 0x30;
	  break;
	default:
	  abort ();
	}

      /* the offset must fit ! after all, what was all the relaxing
	     about ? */

      bfd_put_8 (abfd,
		 bfd_coff_reloc16_get_value (reloc, link_info, input_section),
		 data + dst_address);

      /* Note the magic - src goes up by two bytes, but dst by only
	     one */
      dst_address += 1;
      src_address += 3;

      break;

    case R_JMP2:
      
      /* Special relaxed type */
      {
	bfd_vma dot = link_order->offset
	+ dst_address
	+ link_order->u.indirect.section->output_section->vma;

	int gap = (bfd_coff_reloc16_get_value (reloc, link_info, input_section)
		   - dot - 1);

	if ((gap & ~0xff) != 0 && ((gap & 0xff00) != 0xff00))
	  abort ();

	bfd_put_8 (abfd, gap, data + dst_address);

	switch (data[dst_address - 1])
	  {
	  case 0x5e:
	    /* jsr -> bsr */
	    bfd_put_8 (abfd, 0x55, data + dst_address - 1);
	    break;
	  case 0x5a:
	    /* jmp ->bra */
	    bfd_put_8 (abfd, 0x40, data + dst_address - 1);
	    break;

	  default:
	    abort ();
	  }
	dst_address++;
	src_address += 3;

	break;
      }
      break;

    case R_PCRWORD_B:
      
      /* Special relaxed type */
      {
	bfd_vma dot = link_order->offset
	+ dst_address
	+ link_order->u.indirect.section->output_section->vma - 2;

	int gap = (bfd_coff_reloc16_get_value (reloc, link_info, input_section)
		   - dot - 1);

	if ((gap & ~0xff) != 0 && ((gap & 0xff00) != 0xff00))
	  abort ();

	switch (data[dst_address - 2])
	  {
	  int tmp;

	  case 0x58:
	    /* bCC:16 -> bCC:8 */
	    /* Get the condition code from the original insn.  */
	    tmp = data[dst_address - 1];
	    tmp &= 0xf0;
	    tmp >>= 4;

	    /* Now or in the high nibble of the opcode.  */
	    tmp |= 0x40;

	    /* Write it.  */
	    bfd_put_8 (abfd, tmp, data + dst_address - 2);
	    break;

	  default:
	    abort ();
	  }

	/* Output the target.  */
	bfd_put_8 (abfd, gap, data + dst_address - 1);

	/* We don't advance dst_address -- the 8bit reloc is applied at
	   dst_address - 1, so the next insn should begin at dst_address.

	   src_address is advanced by two (original reloc was 16bits).  */
	src_address += 2;

	break;
      }
      break;
      
    case R_JMPL_B8: /* 24 bit branch which is now 8 bits */
      
      /* Speciial relaxed type */
      {
	bfd_vma dot = link_order->offset
	+ dst_address
	+ link_order->u.indirect.section->output_section->vma;

	int gap = (bfd_coff_reloc16_get_value (reloc, link_info, input_section)
		   - dot - 2);

	if ((gap & ~0xff) != 0 && ((gap & 0xff00) != 0xff00))
	  abort ();

	switch (data[src_address])
	  {
	  case 0x5e:
	    /* jsr -> bsr */
	    bfd_put_8 (abfd, 0x55, data + dst_address);
	    break;
	  case 0x5a:
	    /* jmp ->bra */
	    bfd_put_8 (abfd, 0x40, data + dst_address);
	    break;

	  default:
	    bfd_put_8 (abfd, 0xde, data + dst_address);
	    break;
	  }

	bfd_put_8 (abfd, gap, data + dst_address + 1);
	dst_address += 2;
	src_address += 4;

	break;
      }

    case R_JMPL1:
      {
	int v = bfd_coff_reloc16_get_value (reloc, link_info, input_section);
	int o = bfd_get_32 (abfd, data + src_address);
	v = (v & 0x00ffffff) | (o & 0xff000000);
	bfd_put_32 (abfd, v, data + dst_address);
	dst_address += 4;
	src_address += 4;
      }

      break;


      /* A 24 bit mov  which could be an 8 bit move, really pointing to
	 the byte before the 24bit hole, so we can treat it as a 32bit pointer */
    case R_MOVLB1:
      {
	int v = bfd_coff_reloc16_get_value (reloc, link_info, input_section);
	int o = bfd_get_32 (abfd, data + dst_address);
	v = (v & 0x00ffffff) | (o & 0xff000000);
	bfd_put_32 (abfd, v, data + dst_address);
	dst_address += 4;
	src_address += 4;
      }

      break;

    /* An 8bit memory indirect instruction (jmp/jsr).

       There's several things that need to be done to handle
       this relocation.

       If this is a reloc against the absolute symbol, then
       we should handle it just R_RELBYTE.  Likewise if it's
       for a symbol with a value ge 0 and le 0xff.

       Otherwise it's a jump/call through the function vector,
       and the linker is expected to set up the function vector
       and put the right value into the jump/call instruction.  */
    case R_MEM_INDIRECT:
      {
	/* We need to find the symbol so we can determine it's
	   address in the function vector table.  */
	asymbol *symbol;
	bfd_vma value;
	char *name;
	struct funcvec_hash_entry *h;
	asection *vectors_sec = h8300_coff_hash_table (link_info)->vectors_sec;

	/* First see if this is a reloc against the absolute symbol
	   or against a symbol with a nonnegative value <= 0xff.  */
	symbol = *(reloc->sym_ptr_ptr);
	value = bfd_coff_reloc16_get_value (reloc, link_info, input_section);
	if (symbol == bfd_abs_section_ptr->symbol
	    || (value >= 0 && value <= 0xff))
	  {
	    /* This should be handled in a manner very similar to
	       R_RELBYTES.   If the value is in range, then just slam
	       the value into the right location.  Else trigger a
	       reloc overflow callback.  */
	    if (value >= 0 && value <= 0xff)
	      {
		bfd_put_8 (abfd, value, data + dst_address);
		dst_address += 1;
		src_address += 1;
	      }
	    else
	      {
		if (! ((*link_info->callbacks->reloc_overflow)
		       (link_info, bfd_asymbol_name (*reloc->sym_ptr_ptr),
			reloc->howto->name, reloc->addend, input_section->owner,
			input_section, reloc->address)))
		  abort ();
	      }
	    break;
	  }

	/* This is a jump/call through a function vector, and we're
	   expected to create the function vector ourselves. 

	   First look up this symbol in the linker hash table -- we need
	   the derived linker symbol which holds this symbol's index
	   in the function vector.  */
	name = symbol->name;
	if (symbol->flags & BSF_LOCAL)
	  {
	    char *new_name = bfd_malloc (strlen (name) + 9);
	    if (new_name == NULL)
	      abort ();

	    strcpy (new_name, name);
	    sprintf (new_name + strlen (name), "_%08x",
		     (int)symbol->section);
	    name = new_name;
	  }

	h = funcvec_hash_lookup (h8300_coff_hash_table (link_info)->funcvec_hash_table,
				 name, false, false);

	/* This shouldn't ever happen.  If it does that means we've got
	   data corruption of some kind.  Aborting seems like a reasonable
	   think to do here.  */
	if (h == NULL || vectors_sec == NULL)
	  abort ();

	/* Place the address of the function vector entry into the
	   reloc's address.  */
	bfd_put_8 (abfd,
		   vectors_sec->output_offset + h->offset,
		   data + dst_address);

	dst_address++;
	src_address++;

	/* Now create an entry in the function vector itself.  */
	if (bfd_get_mach (input_section->owner) == bfd_mach_h8300)
	  bfd_put_16 (abfd,
		      bfd_coff_reloc16_get_value (reloc,
						  link_info,
						  input_section),
		      vectors_sec->contents + h->offset);
	else if (bfd_get_mach (input_section->owner) == bfd_mach_h8300h)
	  bfd_put_32 (abfd,
		      bfd_coff_reloc16_get_value (reloc,
						  link_info,
						  input_section),
		      vectors_sec->contents + h->offset);
	else
	  abort ();

	/* Gross.  We've already written the contents of the vector section
	   before we get here...  So we write it again with the new data.  */
	bfd_set_section_contents (vectors_sec->output_section->owner,
				  vectors_sec->output_section,
				  vectors_sec->contents,
				  vectors_sec->output_offset,
				  vectors_sec->_raw_size);
	break;
      }

    default:
      abort ();
      break;

    }

  *src_ptr = src_address;
  *dst_ptr = dst_address;
}


/* Routine for the h8300 linker.

   This routine is necessary to handle the special R_MEM_INDIRECT
   relocs on the h8300.  It's responsible for generating a vectors
   section and attaching it to an input bfd as well as sizing
   the vectors section.  It also creates our vectors hash table.

   It uses the generic linker routines to actually add the symbols.
   from this BFD to the bfd linker hash table.  It may add a few
   selected static symbols to the bfd linker hash table.  */

static boolean
h8300_bfd_link_add_symbols(abfd, info)
     bfd *abfd;
     struct bfd_link_info *info;
{
  asection *sec;
  struct funcvec_hash_table *funcvec_hash_table;

  /* If we haven't created a vectors section, do so now.  */
  if (!h8300_coff_hash_table (info)->vectors_sec)
    {
      flagword flags;

      /* Make sure the appropriate flags are set, including SEC_IN_MEMORY.  */
      flags = (SEC_ALLOC | SEC_LOAD
	       | SEC_HAS_CONTENTS | SEC_IN_MEMORY | SEC_READONLY);
      h8300_coff_hash_table (info)->vectors_sec = bfd_make_section (abfd,
								    ".vectors");

      /* If the section wasn't created, or we couldn't set the flags,
	 quit quickly now, rather than dieing a painful death later.  */
      if (! h8300_coff_hash_table (info)->vectors_sec
	  || ! bfd_set_section_flags (abfd,
				      h8300_coff_hash_table(info)->vectors_sec,
				      flags))
	return false;

      /* Also create the vector hash table.  */
      funcvec_hash_table = ((struct funcvec_hash_table *)
	bfd_alloc (abfd, sizeof (struct funcvec_hash_table)));

      if (!funcvec_hash_table)
	return false;

      /* And initialize the funcvec hash table.  */
      if (!funcvec_hash_table_init (funcvec_hash_table, abfd,
				    funcvec_hash_newfunc))
	{
	  bfd_release (abfd, funcvec_hash_table);
	  return false;
	}

      /* Store away a pointer to the funcvec hash table.  */
      h8300_coff_hash_table (info)->funcvec_hash_table = funcvec_hash_table;
    }

  /* Load up the function vector hash table.  */
  funcvec_hash_table = h8300_coff_hash_table (info)->funcvec_hash_table;

  /* Add the symbols using the generic code.  */
  _bfd_generic_link_add_symbols (abfd, info);

  /* Now scan the relocs for all the sections in this bfd; create
     additional space in the .vectors section as needed.  */
  for (sec = abfd->sections; sec; sec = sec->next)
    {
      unsigned long reloc_size, reloc_count, i;
      asymbol **symbols;
      arelent **relocs;

      /* Suck in the relocs, symbols & canonicalize them.  */
      reloc_size = bfd_get_reloc_upper_bound (abfd, sec);
      if (reloc_size <= 0)
	continue;

      relocs = (arelent **)bfd_malloc ((size_t)reloc_size);
      if (!relocs)
	return false;

      /* The symbols should have been read in by _bfd_generic link_add_symbols
	 call abovec, so we can cheat and use the pointer to them that was
	 saved in the above call.  */
      symbols = _bfd_generic_link_get_symbols(abfd);
      reloc_count = bfd_canonicalize_reloc (abfd, sec, relocs, symbols);

      /* Now walk through all the relocations in this section.  */
      for (i = 0; i < reloc_count; i++)
	{
	  arelent *reloc = relocs[i];
	  asymbol *symbol = *(reloc->sym_ptr_ptr);
	  char *name;

	  /* We've got an indirect reloc.  See if we need to add it
	     to the function vector table.   At this point, we have
	     to add a new entry for each unique symbol referenced
	     by an R_MEM_INDIRECT relocation except for a reloc
	     against the absolute section symbol.  */
	  if (reloc->howto->type == R_MEM_INDIRECT
	      && symbol != bfd_abs_section_ptr->symbol)

	    {
	      struct funcvec_hash_entry *h;

	      name = symbol->name;
	      if (symbol->flags & BSF_LOCAL)
		{
		  char *new_name = bfd_malloc (strlen (name) + 9);

		  if (new_name == NULL)
		    abort ();

		  strcpy (new_name, name);
		  sprintf (new_name + strlen (name), "_%08x",
			   (int)symbol->section);
		  name = new_name;
		}

	      /* Look this symbol up in the function vector hash table.  */
	      h = funcvec_hash_lookup (h8300_coff_hash_table (info)->funcvec_hash_table,
				       name, false, false);


	      /* If this symbol isn't already in the hash table, add
		 it and bump up the size of the hash table.  */
	      if (h == NULL)
		{
		  h = funcvec_hash_lookup (h8300_coff_hash_table (info)->funcvec_hash_table,
					   name, true, true);
		  if (h == NULL)
		    {
		      free (relocs);
		      return false;
		    }

		  /* Bump the size of the vectors section.  Each vector
		     takes 2 bytes on the h8300 and 4 bytes on the h8300h.  */
		  if (bfd_get_mach (abfd) == bfd_mach_h8300)
		    h8300_coff_hash_table (info)->vectors_sec->_raw_size += 2;
		  else if (bfd_get_mach (abfd) == bfd_mach_h8300h)
		    h8300_coff_hash_table (info)->vectors_sec->_raw_size += 4;
		}
	    }
	}

      /* We're done with the relocations, release them.  */
      free (relocs);
    }

  /* Now actually allocate some space for the function vector.  It's
     wasteful to do this more than once, but this is easier.  */
  if (h8300_coff_hash_table (info)->vectors_sec->_raw_size != 0)
    {
      /* Free the old contents.  */
      if (h8300_coff_hash_table (info)->vectors_sec->contents)
	free (h8300_coff_hash_table (info)->vectors_sec->contents);

      /* Allocate new contents.  */
      h8300_coff_hash_table (info)->vectors_sec->contents
	= bfd_malloc (h8300_coff_hash_table (info)->vectors_sec->_raw_size);
    }

  return true;
}

#define coff_reloc16_extra_cases h8300_reloc16_extra_cases
#define coff_reloc16_estimate h8300_reloc16_estimate
#define coff_bfd_link_add_symbols h8300_bfd_link_add_symbols
#define coff_bfd_link_hash_table_create h8300_coff_link_hash_table_create

#define COFF_LONG_FILENAMES
#include "coffcode.h"


#undef coff_bfd_get_relocated_section_contents
#undef coff_bfd_relax_section
#define coff_bfd_get_relocated_section_contents \
  bfd_coff_reloc16_get_relocated_section_contents
#define coff_bfd_relax_section bfd_coff_reloc16_relax_section



const bfd_target h8300coff_vec =
{
  "coff-h8300",			/* name */
  bfd_target_coff_flavour,
  BFD_ENDIAN_BIG,		/* data byte order is big */
  BFD_ENDIAN_BIG,		/* header byte order is big */

  (HAS_RELOC | EXEC_P |		/* object flags */
   HAS_LINENO | HAS_DEBUG |
   HAS_SYMS | HAS_LOCALS | WP_TEXT | BFD_IS_RELAXABLE ),
  (SEC_HAS_CONTENTS | SEC_ALLOC | SEC_LOAD | SEC_RELOC),	/* section flags */
  '_',				/* leading char */
  '/',				/* ar_pad_char */
  15,				/* ar_max_namelen */
  bfd_getb64, bfd_getb_signed_64, bfd_putb64,
  bfd_getb32, bfd_getb_signed_32, bfd_putb32,
  bfd_getb16, bfd_getb_signed_16, bfd_putb16,	/* data */
  bfd_getb64, bfd_getb_signed_64, bfd_putb64,
  bfd_getb32, bfd_getb_signed_32, bfd_putb32,
  bfd_getb16, bfd_getb_signed_16, bfd_putb16,	/* hdrs */

  {_bfd_dummy_target, coff_object_p,	/* bfd_check_format */
   bfd_generic_archive_p, _bfd_dummy_target},
  {bfd_false, coff_mkobject, _bfd_generic_mkarchive,	/* bfd_set_format */
   bfd_false},
  {bfd_false, coff_write_object_contents,	/* bfd_write_contents */
   _bfd_write_archive_contents, bfd_false},

     BFD_JUMP_TABLE_GENERIC (coff),
     BFD_JUMP_TABLE_COPY (coff),
     BFD_JUMP_TABLE_CORE (_bfd_nocore),
     BFD_JUMP_TABLE_ARCHIVE (_bfd_archive_coff),
     BFD_JUMP_TABLE_SYMBOLS (coff),
     BFD_JUMP_TABLE_RELOCS (coff),
     BFD_JUMP_TABLE_WRITE (coff),
     BFD_JUMP_TABLE_LINK (coff),
     BFD_JUMP_TABLE_DYNAMIC (_bfd_nodynamic),

  COFF_SWAP_TABLE,
};

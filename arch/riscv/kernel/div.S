/* Integer division routines for RISC-V.

   Copyright (C) 2016-2021 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

//https://github.com/gcc-mirror/gcc/blob/master/libgcc/config/riscv/div.S

//#include "riscv-asm.h"

#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/asm.h>
#include <asm/csr.h>
#include <asm/unistd.h>
#include <asm/thread_info.h>
#include <asm/asm-offsets.h>


#if __riscv_xlen == 32
/* Our RV64 64-bit routines are equivalent to our RV32 32-bit routines.  */
// # define _m_udivdi3 _m_udivsi3
// # define _m_umoddi3 _m_umodsi3
// # define _m_divdi3 _m_divsi3
// # define _m_moddi3 _m_modsi3
#else
ENTRY (_m_udivsi3)
  /* Compute _m_udivdi3(a0 << 32, a1 << 32); cast result to uint32_t.  */
  sll    a0, a0, 32
  sll    a1, a1, 32
  move   t0, ra
  jal    _m_udivdi3
  sext.w a0, a0
  jr     t0
END (_m_udivsi3)

ENTRY (_m_umodsi3)
  /* Compute _m_udivdi3((uint32_t)a0, (uint32_t)a1); cast a1 to uint32_t.  */
  sll    a0, a0, 32
  sll    a1, a1, 32
  srl    a0, a0, 32
  srl    a1, a1, 32
  move   t0, ra
  jal    _m_udivdi3
  sext.w a0, a1
  jr     t0
END (_m_umodsi3)

// FUNC_ALIAS (_m_modsi3, _m_moddi3)

ENTRY( _m_divsi3)
  /* Check for special case of INT_MIN/-1. Otherwise, fall into _m_divdi3.  */
  li    t0, -1
  beq   a1, t0, .L20
#endif

ENTRY (_m_divdi3)
  bltz  a0, .L10
  bltz  a1, .L11
  /* Since the quotient is positive, fall into _m_udivdi3.  */

ENTRY (_m_udivdi3)
  mv    a2, a1
  mv    a1, a0
  li    a0, -1
  beqz  a2, .L5
  li    a3, 1
  bgeu  a2, a1, .L2
.L1:
  blez  a2, .L2
  slli  a2, a2, 1
  slli  a3, a3, 1
  bgtu  a1, a2, .L1
.L2:
  li    a0, 0
.L3:
  bltu  a1, a2, .L4
  sub   a1, a1, a2
  or    a0, a0, a3
.L4:
  srli  a3, a3, 1
  srli  a2, a2, 1
  bnez  a3, .L3
.L5:
  ret
END (_m_udivdi3)

ENTRY (_m_umoddi3)
  /* Call _m_udivdi3(a0, a1), then return the remainder, which is in a1.  */
  move  t0, ra
  jal   _m_udivdi3
  move  a0, a1
  jr    t0
END (_m_umoddi3)

  /* Handle negative arguments to _m_divdi3.  */
.L10:
  neg   a0, a0
  /* Zero is handled as a negative so that the result will not be inverted.  */
  bgtz  a1, .L12     /* Compute _m_udivdi3(-a0, a1), then negate the result.  */

  neg   a1, a1
  j     _m_udivdi3    /* Compute _m_udivdi3(-a0, -a1).  */
.L11:                /* Compute _m_udivdi3(a0, -a1), then negate the result.  */
  neg   a1, a1
.L12:
  move  t0, ra
  jal   _m_udivdi3
  neg   a0, a0
  jr    t0
END (_m_divdi3)

ENTRY (_m_moddi3)
  move   t0, ra
  bltz   a1, .L31
  bltz   a0, .L32
.L30:
  jal    _m_udivdi3    /* The dividend is not negative.  */
  move   a0, a1
  jr     t0
.L31:
  neg    a1, a1
  bgez   a0, .L30
.L32:
  neg    a0, a0
  jal    _m_udivdi3    /* The dividend is hella negative.  */
  neg    a0, a1
  jr     t0
END (_m_moddi3)

#if __riscv_xlen == 64
  /* continuation of _m_divsi3 */
.L20:
  sll   t0, t0, 31
  bne   a0, t0, _m_divdi3
  ret
END (_m_divsi3)
#endif

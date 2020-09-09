/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * i386 target-specific operand constaints.
 * Copyright (c) 2020 Linaro
 */

C_O0_I1(r)

C_O0_I2(qi, r)
C_O0_I2(ri, r)
C_O0_I2(re, r)
C_O0_I2(r, re)
C_O0_I2(L, L)
C_O0_I2(x, r)

C_O0_I3(L, L, L)

C_O0_I4(L, L, L, L)
C_O0_I4(r, r, ri, ri)

C_O1_I1(r, 0)
C_O1_I1(r, q)
C_O1_I1(r, r)
C_O1_I1(r, L)
C_O1_I1(x, r)
C_O1_I1(x, x)

C_O1_I2(r, r, re)
C_O1_I2(r, 0, r)
C_O1_I2(r, 0, re)
C_O1_I2(r, 0, reZ)
C_O1_I2(r, 0, rI)
C_O1_I2(r, 0, ri)
C_O1_I2(r, 0, ci)
C_O1_I2(r, r, ri)
C_O1_I2(Q, 0, Q)
C_O1_I2(q, r, re)
C_O1_I2(r, L, L)
C_O1_I2(x, x, x)
C_N1_I2(r, r, r)
C_N1_I2(r, r, rW)

C_O1_I3(x, x, x, x)

C_O1_I4(r, r, re, r, 0)
C_O1_I4(r, r, r, ri, ri)

C_O2_I1(r, r, L)

C_O2_I2(r, r, L, L)
C_O2_I2(a, d, a, r)

C_O2_I3(a, d, 0, 1, r)

C_O2_I4(r, r, 0, 1, re, re)

/*
 * No host specific aes acceleration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HOST_AES_ROUND_H
#define HOST_AES_ROUND_H

#define HAVE_AES_ACCEL  false
#define ATTR_AES_ACCEL

void aesenc_MC_accel(AESState *, const AESState *, bool)
    QEMU_ERROR("unsupported accel");

void aesenc_SB_SR_accel(AESState *, const AESState *, bool)
    QEMU_ERROR("unsupported accel");

void aesenc_SB_SR_MC_AK_accel(AESState *, const AESState *,
                              const AESState *, bool)
    QEMU_ERROR("unsupported accel");

void aesdec_IMC_accel(AESState *, const AESState *, bool)
    QEMU_ERROR("unsupported accel");

void aesdec_ISB_ISR_accel(AESState *, const AESState *, bool)
    QEMU_ERROR("unsupported accel");

void aesdec_ISB_ISR_IMC_AK_accel(AESState *, const AESState *,
                                 const AESState *, bool)
    QEMU_ERROR("unsupported accel");

#endif

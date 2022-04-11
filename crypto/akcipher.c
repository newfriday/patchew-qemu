/*
 * QEMU Crypto akcipher algorithms
 *
 * Copyright (c) 2022 Bytedance
 * Author: zhenwei pi <pizhenwei@bytedance.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "crypto/akcipher.h"
#include "akcipherpriv.h"

QCryptoAkCipher *qcrypto_akcipher_new(const QCryptoAkCipherOptions *opts,
                                      QCryptoAkCipherKeyType type,
                                      const uint8_t *key, size_t keylen,
                                      Error **errp)
{
    QCryptoAkCipher *akcipher = NULL;

    return akcipher;
}

bool qcrypto_akcipher_supports(QCryptoAkCipherOptions *opts)
{
    return false;
}

int qcrypto_akcipher_encrypt(QCryptoAkCipher *akcipher,
                             const void *in, size_t in_len,
                             void *out, size_t out_len, Error **errp)
{
    const QCryptoAkCipherDriver *drv = akcipher->driver;

    return drv->encrypt(akcipher, in, in_len, out, out_len, errp);
}

int qcrypto_akcipher_decrypt(QCryptoAkCipher *akcipher,
                             const void *in, size_t in_len,
                             void *out, size_t out_len, Error **errp)
{
    const QCryptoAkCipherDriver *drv = akcipher->driver;

    return drv->decrypt(akcipher, in, in_len, out, out_len, errp);
}

int qcrypto_akcipher_sign(QCryptoAkCipher *akcipher,
                          const void *in, size_t in_len,
                          void *out, size_t out_len, Error **errp)
{
    const QCryptoAkCipherDriver *drv = akcipher->driver;

    return drv->sign(akcipher, in, in_len, out, out_len, errp);
}

int qcrypto_akcipher_verify(QCryptoAkCipher *akcipher,
                            const void *in, size_t in_len,
                            const void *in2, size_t in2_len, Error **errp)
{
    const QCryptoAkCipherDriver *drv = akcipher->driver;

    return drv->verify(akcipher, in, in_len, in2, in2_len, errp);
}

int qcrypto_akcipher_max_plaintext_len(QCryptoAkCipher *akcipher)
{
    return akcipher->max_plaintext_len;
}

int qcrypto_akcipher_max_ciphertext_len(QCryptoAkCipher *akcipher)
{
    return akcipher->max_ciphertext_len;
}

int qcrypto_akcipher_max_signature_len(QCryptoAkCipher *akcipher)
{
    return akcipher->max_signature_len;
}

int qcrypto_akcipher_max_dgst_len(QCryptoAkCipher *akcipher)
{
    return akcipher->max_dgst_len;
}

int qcrypto_akcipher_free(QCryptoAkCipher *akcipher, Error **errp)
{
    const QCryptoAkCipherDriver *drv = akcipher->driver;

    return drv->free(akcipher, errp);
}

/*
 * QEMU Crypto cipher speed benchmark
 *
 * Copyright (c) 2022 Bytedance
 *
 * Authors:
 *    lei he <helei.sig11@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"
#include "crypto/init.h"
#include "crypto/akcipher.h"
#include "standard-headers/linux/virtio_crypto.h"

#include "test_akcipher_keys.inc"

static bool keep_running;

static void alarm_handler(int sig)
{
    keep_running = false;
}

static QCryptoAkcipher *create_rsa_akcipher(const uint8_t *priv_key,
                                            size_t keylen,
                                            QCryptoRsaPaddingAlgorithm padding,
                                            QCryptoRsaHashAlgorithm hash)
{
    QCryptoRsaOptions opt;
    QCryptoAkcipher *rsa;
    Error *err = NULL;

    opt.padding_algo = padding;
    opt.hash_algo = hash;
    rsa = qcrypto_akcipher_new(QCRYPTO_AKCIPHER_ALG_RSA,
                               QCRYPTO_AKCIPHER_KEY_TYPE_PRIVATE,
                               priv_key, keylen, &opt, &err);

    g_assert(rsa != NULL);
    return rsa;
}

static void test_rsa_speed(const uint8_t *priv_key, size_t keylen,
                           size_t key_size)
{
#define Byte 8
#define SHA1_DGST_LEN 40
#define DURATION_SECONDS 10
#define padding QCRYPTO_RSA_PADDING_ALG_PKCS1
#define hash QCRYPTO_RSA_HASH_ALG_SHA1

    Error *err = NULL;
    QCryptoAkcipher *rsa;
    uint8_t *dgst, *signature;
    size_t count;

    rsa = create_rsa_akcipher(priv_key, keylen, padding, hash);

    dgst = g_new0(uint8_t, SHA1_DGST_LEN);
    memset(dgst, g_test_rand_int(), SHA1_DGST_LEN);
    signature = g_new0(uint8_t, key_size / Byte);

    g_test_message("benchmark rsa%lu (%s-%s) sign in %d seconds", key_size,
                   QCryptoRsaPaddingAlgorithm_str(padding),
                   QCryptoRsaHashAlgorithm_str(hash),
                   DURATION_SECONDS);
    alarm(DURATION_SECONDS);
    g_test_timer_start();
    for (keep_running = true, count = 0; keep_running; ++count) {
        g_assert(qcrypto_akcipher_sign(rsa, dgst, SHA1_DGST_LEN,
                                       signature, key_size / Byte, &err) > 0);
    }
    g_test_timer_elapsed();
    g_test_message("rsa%lu (%s-%s) sign %lu times in %.2f seconds,"
                   " %.2f times/sec ",
                   key_size,  QCryptoRsaPaddingAlgorithm_str(padding),
                   QCryptoRsaHashAlgorithm_str(hash),
                   count, g_test_timer_last(),
                   (double)count / g_test_timer_last());

    g_test_message("benchmark rsa%lu (%s-%s) verify in %d seconds", key_size,
                   QCryptoRsaPaddingAlgorithm_str(padding),
                   QCryptoRsaHashAlgorithm_str(hash),
                   DURATION_SECONDS);
    alarm(DURATION_SECONDS);
    g_test_timer_start();
    for (keep_running = true, count = 0; keep_running; ++count) {
        g_assert(qcrypto_akcipher_verify(rsa, signature, key_size / Byte,
                                         dgst, SHA1_DGST_LEN, &err) == 0);
    }
    g_test_timer_elapsed();
    g_test_message("rsa%lu (%s-%s) verify %lu times in %.2f seconds,"
                   " %.2f times/sec ",
                   key_size, QCryptoRsaPaddingAlgorithm_str(padding),
                   QCryptoRsaHashAlgorithm_str(hash),
                   count, g_test_timer_last(),
                   (double)count / g_test_timer_last());

    g_assert(qcrypto_akcipher_free(rsa, &err) == 0);
    g_free(dgst);
    g_free(signature);
}

static void test_rsa_1024_speed(const void *opaque)
{
    size_t key_size = (size_t)opaque;
    test_rsa_speed(rsa1024_priv_key, sizeof(rsa1024_priv_key), key_size);
}

static void test_rsa_2048_speed(const void *opaque)
{
    size_t key_size = (size_t)opaque;
    test_rsa_speed(rsa2048_priv_key, sizeof(rsa2048_priv_key), key_size);
}

static void test_rsa_4096_speed(const void *opaque)
{
    size_t key_size = (size_t)opaque;
    test_rsa_speed(rsa4096_priv_key, sizeof(rsa4096_priv_key), key_size);
}

int main(int argc, char **argv)
{
    char *alg = NULL;
    char *size = NULL;
    g_test_init(&argc, &argv, NULL);
    g_assert(qcrypto_init(NULL) == 0);
    struct sigaction new_action, old_action;

    new_action.sa_handler = alarm_handler;

    /* Set up the structure to specify the new action. */
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    sigaction(SIGALRM, NULL, &old_action);
    g_assert(old_action.sa_handler != SIG_IGN);
    sigaction(SIGALRM, &new_action, NULL);

#define ADD_TEST(asym_alg, keysize)                    \
    if ((!alg || g_str_equal(alg, #asym_alg)) &&       \
        (!size || g_str_equal(size, #keysize)))        \
        g_test_add_data_func(                          \
        "/crypto/akcipher/" #asym_alg "-" #keysize,    \
        (void *)keysize,                               \
        test_ ## asym_alg ## _ ## keysize ## _speed)

    if (argc >= 2) {
        alg = argv[1];
    }
    if (argc >= 3) {
        size = argv[2];
    }

    ADD_TEST(rsa, 1024);
    ADD_TEST(rsa, 2048);
    ADD_TEST(rsa, 4096);

    return g_test_run();
}

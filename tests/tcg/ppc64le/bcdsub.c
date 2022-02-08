#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <altivec.h>

#define CRF_LT  (1 << 3)
#define CRF_GT  (1 << 2)
#define CRF_EQ  (1 << 1)
#define CRF_SO  (1 << 0)
#define UNDEF   0

#ifdef __LITTLE_ENDIAN__
#define HIGH(T) (T)[1]
#define LOW(T) (T)[0]
#define U128(H, L) (vector unsigned long long) {L, H}
#else
#define HIGH(T) (T)[0]
#define LOW(T) (T)[1]
#define U128(H, L) (vector unsigned long long) {H, L}
#endif

#define BCDSUB(vra, vrb, ps)                    \
    asm ("bcdsub. %1,%2,%3,%4;"                 \
         "mfocrf %0,0b10;"                      \
         : "=r" (cr), "=v" (vrt)                \
         : "v" (vra), "v" (vrb), "i" (ps)       \
         : );

#define TEST(vra, vrb, ps, exp_res_h, exp_res_l, exp_cr6)   \
    do {                                                    \
        vector unsigned long long vrt = U128(0, 0);         \
        int cr = 0;                                         \
        BCDSUB(vra, vrb, ps);                               \
        if (exp_res_h || exp_res_l) {                       \
            assert(HIGH(vrt) == exp_res_h);                 \
            assert(LOW(vrt) == exp_res_l);                  \
        }                                                   \
        assert((cr >> 4) == exp_cr6);                       \
    } while (0)

/*
 * Unbounded result is equal to zero:
 *   sign = (PS) ? 0b1111 : 0b1100
 *   CR6 = 0b0010
 */
void test_bcdsub_eq(void)
{
    vector unsigned long long a, b;

    /* maximum positive BCD value */
    a = b = U128(0x9999999999999999, 0x999999999999999c);

    TEST(a, b, 0, 0x0, 0xc, CRF_EQ);
    TEST(a, b, 1, 0x0, 0xf, CRF_EQ);
}

/*
 * Unbounded result is greater than zero:
 *   sign = (PS) ? 0b1111 : 0b1100
 *   CR6 = (overflow) ? 0b0101 : 0b0100
 */
void test_bcdsub_gt(void)
{
    vector unsigned long long a, b, c;

    /* maximum positive BCD value */
    a = U128(0x9999999999999999, 0x999999999999999c);

    /* negative one BCD value */
    b = U128(0x0, 0x1d);

    TEST(a, b, 0, 0x0, 0xc, (CRF_GT | CRF_SO));
    TEST(a, b, 1, 0x0, 0xf, (CRF_GT | CRF_SO));

    c = U128(0x9999999999999999, 0x999999999999998c);

    TEST(c, b, 0, HIGH(a), LOW(a), CRF_GT);
    TEST(c, b, 1, HIGH(a), (LOW(a) | 0x3), CRF_GT);
}

/*
 * Unbounded result is less than zero:
 *   sign = 0b1101
 *   CR6 = (overflow) ? 0b1001 : 0b1000
 */
void test_bcdsub_lt(void)
{
    vector unsigned long long a, b;

    /* positive zero BCD value */
    a = U128(0x0, 0xc);

    /* positive one BCD value */
    b = U128(0x0, 0x1c);

    TEST(a, b, 0, 0x0, 0x1d, CRF_LT);
    TEST(a, b, 1, 0x0, 0x1d, CRF_LT);

    /* maximum negative BCD value */
    a = U128(0x9999999999999999, 0x999999999999999d);

    /* positive one BCD value */
    b = U128(0x0, 0x1c);

    TEST(a, b, 0, 0x0, 0xd, (CRF_LT | CRF_SO));
    TEST(a, b, 1, 0x0, 0xd, (CRF_LT | CRF_SO));
}

void test_bcdsub_invalid(void)
{
    vector unsigned long long a, b;

    /* positive one BCD value */
    a = U128(0x0, 0x1c);
    b = U128(0x0, 0xf00);

    TEST(a, b, 0, UNDEF, UNDEF, CRF_SO);
    TEST(a, b, 1, UNDEF, UNDEF, CRF_SO);

    TEST(b, a, 0, UNDEF, UNDEF, CRF_SO);
    TEST(b, a, 1, UNDEF, UNDEF, CRF_SO);

    a = U128(0x0, 0xbad);

    TEST(a, b, 0, UNDEF, UNDEF, CRF_SO);
    TEST(a, b, 1, UNDEF, UNDEF, CRF_SO);
}

int main(void)
{
    struct sigaction action;

    action.sa_handler = _exit;
    sigaction(SIGABRT, &action, NULL);

    test_bcdsub_eq();
    test_bcdsub_gt();
    test_bcdsub_lt();
    test_bcdsub_invalid();

    return 0;
}

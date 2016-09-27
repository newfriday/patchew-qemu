#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qint.h"
#include "libqtest.h"

static char *get_cpu0_qom_path(void)
{
    QDict *resp;
    QList *ret;
    QDict *cpu0;
    char *path;

    resp = qmp("{'execute': 'query-cpus', 'arguments': {}}");
    g_assert(qdict_haskey(resp, "return"));
    ret = qdict_get_qlist(resp, "return");

    cpu0 = qobject_to_qdict(qlist_peek(ret));
    path = g_strdup(qdict_get_str(cpu0, "qom_path"));
    QDECREF(resp);
    return path;
}

static QObject *qom_get(const char *path, const char *prop)
{
    QDict *resp = qmp("{ 'execute': 'qom-get',"
                      "  'arguments': { 'path': %s,"
                      "                 'property': %s } }",
                      path, prop);
    QObject *ret = qdict_get(resp, "return");
    qobject_incref(ret);
    QDECREF(resp);
    return ret;
}

typedef struct CpuidTestArgs {
    const char *cmdline;
    const char *property;
    int64_t expected_value;
} CpuidTestArgs;

static void test_cpuid_prop(const void *data)
{
    const CpuidTestArgs *args = data;
    char *path;
    QInt *value;

    qtest_start(args->cmdline);
    path = get_cpu0_qom_path();
    value = qobject_to_qint(qom_get(path, args->property));
    g_assert_cmpint(qint_get_int(value), ==, args->expected_value);
    qtest_end();

    QDECREF(value);
    g_free(path);
}

static void add_cpuid_test(const char *name, const char *cmdline,
                           const char *property, int64_t expected_value)
{
    CpuidTestArgs *args = g_new0(CpuidTestArgs, 1);
    args->cmdline = cmdline;
    args->property = property;
    args->expected_value = expected_value;
    qtest_add_data_func(name, args, test_cpuid_prop);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Original level values for CPU models: */
    add_cpuid_test("x86/cpuid/phenom/level",
                   "-cpu phenom", "level", 5);
    add_cpuid_test("x86/cpuid/Conroe/level",
                   "-cpu Conroe", "level", 10);
    add_cpuid_test("x86/cpuid/SandyBridge/level",
                   "-cpu SandyBridge", "level", 0xd);
    add_cpuid_test("x86/cpuid/486/xlevel",
                   "-cpu 486", "xlevel", 0);
    add_cpuid_test("x86/cpuid/core2duo/xlevel",
                   "-cpu core2duo", "xlevel", 0x80000008);
    add_cpuid_test("x86/cpuid/phenom/xlevel",
                   "-cpu phenom", "xlevel", 0x8000001A);

    /* If level is not large enough, it should increase automatically: */
    /* CPUID[6].EAX: */
    add_cpuid_test("x86/cpuid/auto-level/phenom/arat",
                   "-cpu 486,+arat", "level", 6);
    /* CPUID[EAX=7,ECX=0].EBX: */
    add_cpuid_test("x86/cpuid/auto-level/phenom/fsgsbase",
                   "-cpu phenom,+fsgsbase", "level", 7);
    /* CPUID[EAX=7,ECX=0].ECX: */
    add_cpuid_test("x86/cpuid/auto-level/phenom/avx512vbmi",
                   "-cpu phenom,+avx512vbmi", "level", 7);
    /* CPUID[EAX=0xd,ECX=1].EAX: */
    add_cpuid_test("x86/cpuid/auto-level/phenom/xsaveopt",
                   "-cpu phenom,+xsaveopt", "level", 0xd);
    /* CPUID[8000_0001].EDX: */
    add_cpuid_test("x86/cpuid/auto-xlevel/486/3dnow",
                   "-cpu 486,+3dnow", "xlevel", 0x80000001);
    /* CPUID[8000_0001].ECX: */
    add_cpuid_test("x86/cpuid/auto-xlevel/486/sse4a",
                   "-cpu 486,+sse4a", "xlevel", 0x80000001);
    /* CPUID[8000_0007].EDX: */
    add_cpuid_test("x86/cpuid/auto-xlevel/486/invtsc",
                   "-cpu 486,+invtsc", "xlevel", 0x80000007);
    /* CPUID[8000_000A].EDX: */
    add_cpuid_test("x86/cpuid/auto-xlevel/486/npt",
                   "-cpu 486,+npt", "xlevel", 0x8000000A);
    /* CPUID[C000_0001].EDX: */
    add_cpuid_test("x86/cpuid/auto-xlevel2/phenom/xstore",
                   "-cpu phenom,+xstore", "xlevel2", 0xC0000001);


    /* If level is already large enough, it shouldn't change: */
    add_cpuid_test("x86/cpuid/auto-level/SandyBridge/multiple",
                   "-cpu SandyBridge,+arat,+fsgsbase,+avx512vbmi",
                   "level", 0xd);
    /* If level is explicitly set, it shouldn't change: */
    add_cpuid_test("x86/cpuid/auto-level/486/fixed/0xF",
                   "-cpu 486,level=0xF,+arat,+fsgsbase,+avx512vbmi,+xsaveopt",
                   "level", 0xF);
    add_cpuid_test("x86/cpuid/auto-level/486/fixed/2",
                   "-cpu 486,level=2,+arat,+fsgsbase,+avx512vbmi,+xsaveopt",
                   "level", 2);
    add_cpuid_test("x86/cpuid/auto-level/486/fixed/0",
                   "-cpu 486,level=0,+arat,+fsgsbase,+avx512vbmi,+xsaveopt",
                   "level", 0);

    /* if xlevel is already large enough, it shouldn't change: */
    add_cpuid_test("x86/cpuid/auto-xlevel/phenom/3dnow",
                   "-cpu phenom,+3dnow,+sse4a,+invtsc,+npt",
                   "xlevel", 0x8000001A);
    /* If xlevel is explicitly set, it shouldn't change: */
    add_cpuid_test("x86/cpuid/auto-xlevel/486/fixed/80000002",
                   "-cpu 486,xlevel=0x80000002,+3dnow,+sse4a,+invtsc,+npt",
                   "xlevel", 0x80000002);
    add_cpuid_test("x86/cpuid/auto-xlevel/486/fixed/8000001A",
                   "-cpu 486,xlevel=0x8000001A,+3dnow,+sse4a,+invtsc,+npt",
                   "xlevel", 0x8000001A);
    add_cpuid_test("x86/cpuid/auto-xlevel/phenom/fixed/0",
                   "-cpu 486,xlevel=0,+3dnow,+sse4a,+invtsc,+npt",
                   "xlevel", 0);

    /* if xlevel2 is already large enough, it shouldn't change: */
    add_cpuid_test("x86/cpuid/auto-xlevel2/486/fixed",
                   "-cpu 486,xlevel2=0xC0000002,+xstore",
                   "xlevel2", 0xC0000002);

    /* Check compatibility of old machine-types that didn't
     * auto-increase level/xlevel/xlevel2: */

    add_cpuid_test("x86/cpuid/auto-level/pc-2.7",
                   "-machine pc-i440fx-2.7 -cpu 486,+arat,+avx512vbmi,+xsaveopt",
                   "level", 1);
    add_cpuid_test("x86/cpuid/auto-xlevel/pc-2.7",
                   "-machine pc-i440fx-2.7 -cpu 486,+3dnow,+sse4a,+invtsc,+npt",
                   "xlevel", 0);
    add_cpuid_test("x86/cpuid/auto-xlevel2/pc-2.7",
                   "-machine pc-i440fx-2.7 -cpu 486,+xstore",
                   "xlevel2", 0);

    return g_test_run();
}

/* gv_test_main.c — the test runner. */
#include "gv_test.h"

#include <stdarg.h>

#define GV_MAX_TESTS 256

typedef struct {
    const char *name;
    gv_test_fn fn;
} gv_test_entry;

static gv_test_entry g_tests[GV_MAX_TESTS];
static int g_test_count;
static int g_current_failures;
static long g_total_checks;
static int g_registration_overflow;

void gv_test_register(const char *name, gv_test_fn fn)
{
    if (g_test_count >= GV_MAX_TESTS) {
        g_registration_overflow++;
        return;
    }
    g_tests[g_test_count].name = name;
    g_tests[g_test_count].fn = fn;
    g_test_count++;
}

void gv_test_pass_check(void)
{
    g_total_checks++;
}

void gv_test_fail(const char *file, int line, const char *fmt, ...)
{
    g_total_checks++;
    g_current_failures++;

    /* Trim the path so failures read as "tests/test_sim.c:123". */
    const char *slash = strrchr(file, '/');
    const char *shown = slash ? slash + 1 : file;

    fprintf(stderr, "    FAIL %s:%d: ", shown, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Sort by name so the report order is stable regardless of the order the
 * linker happened to run the constructors in. */
static int gv_cmp_entry(const void *a, const void *b)
{
    const gv_test_entry *ea = (const gv_test_entry *)a;
    const gv_test_entry *eb = (const gv_test_entry *)b;
    return strcmp(ea->name, eb->name);
}

int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
            if (filter == argv[i]) filter = NULL;
        }
    }

    if (g_registration_overflow > 0) {
        fprintf(stderr, "FATAL: %d tests could not register (raise GV_MAX_TESTS)\n",
                g_registration_overflow);
        return 2;
    }

    qsort(g_tests, (size_t)g_test_count, sizeof(g_tests[0]), gv_cmp_entry);

    int run = 0, failed = 0;
    for (int i = 0; i < g_test_count; ++i) {
        if (filter && strstr(g_tests[i].name, filter) == NULL) continue;

        g_current_failures = 0;
        run++;
        if (verbose) fprintf(stderr, "  RUN  %s\n", g_tests[i].name);
        g_tests[i].fn();

        if (g_current_failures > 0) {
            failed++;
            fprintf(stderr, "  ---- %s: %d failed check(s)\n", g_tests[i].name,
                    g_current_failures);
        } else if (verbose) {
            fprintf(stderr, "  ok   %s\n", g_tests[i].name);
        }
    }

    fprintf(stderr, "\n%s  %d/%d tests passed, %ld checks\n",
            failed == 0 ? "PASS" : "FAIL", run - failed, run, g_total_checks);
    if (run == 0) {
        fprintf(stderr, "no tests matched filter '%s'\n", filter ? filter : "");
        return 2;
    }
    return failed == 0 ? 0 : 1;
}

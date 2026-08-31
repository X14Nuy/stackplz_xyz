#include <stdio.h>

int spz_test_failures;

int test_command(void) __attribute__((weak));
int test_ring(void) __attribute__((weak));
int test_profile(void) __attribute__((weak));
int test_task(void) __attribute__((weak));
int test_debug(void) __attribute__((weak));
int test_arm64_encoding(void) __attribute__((weak));
int test_hooks(void) __attribute__((weak));
int test_async(void) __attribute__((weak));
int test_control(void) __attribute__((weak));
int test_maps(void) __attribute__((weak));
int test_maps_renderer(void) __attribute__((weak));

static void run_suite(const char *name, int (*suite)(void))
{
    int before;

    if (suite == NULL)
        return;
    before = spz_test_failures;
    (void)suite();
    printf("%s: %s\n", name, before == spz_test_failures ? "PASS" : "FAIL");
}

int main(void)
{
    run_suite("test_command", test_command);
    run_suite("test_ring", test_ring);
    run_suite("test_profile", test_profile);
    run_suite("test_task", test_task);
    run_suite("test_debug", test_debug);
    run_suite("test_arm64_encoding", test_arm64_encoding);
    run_suite("test_hooks", test_hooks);
    run_suite("test_async", test_async);
    run_suite("test_control", test_control);
    run_suite("test_maps", test_maps);
    run_suite("test_maps_renderer", test_maps_renderer);
    if (spz_test_failures != 0)
        fprintf(stderr, "%d expectation(s) failed\n", spz_test_failures);
    return spz_test_failures == 0 ? 0 : 1;
}

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "proto/wifi_proto.h"

static void test_socket_path(void **state)
{
    (void)state;
    assert_non_null(WIFI_SOCKET_PATH);
    assert_true(WIFI_SOCKET_PATH[0] == '/');
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_socket_path),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

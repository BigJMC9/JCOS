#include <stdio.h>
#include "../../kernel/tests/polaris_read_profile_test.h"
int main(void) {
    if (!polaris_capture_model_test() || !polaris_read_profile_model_test()) return 1;
    puts("PASS: six ordered finite captures; every expiry boundary, tick wrap, clock/selector/refusal;"
         " all readiness observations retain immutable physical qualification blockers.");
    return 0;
}

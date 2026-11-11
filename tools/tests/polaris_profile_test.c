#include <stdio.h>
#include "../../kernel/tests/polaris_profile_test.h"
int main(void){if(!polaris_profile_test())return 1;puts("PASS: sole compiled profile golden values; exact identity/refused variants; forged descriptors; checked retained layout and overflow; corrupted runtime refused; no device authority or IO.");return 0;}

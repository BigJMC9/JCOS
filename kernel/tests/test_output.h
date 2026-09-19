#ifndef JA_OS_TEST_OUTPUT_H
#define JA_OS_TEST_OUTPUT_H

#include "terminal.h"

static inline void test_output_final(const char *name, bool pass) {
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}

#endif

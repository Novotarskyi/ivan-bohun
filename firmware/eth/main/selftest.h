/* selftest.h - D8: prove-then-mark-valid, or the bootloader rolls you back.
 * See selftest.c for the two incidents that bought this file. */
#pragma once
#include <stdbool.h>

void selftest_start(void);      /* call after serve_start() */
bool selftest_on_trial(void);   /* true while this image is still unproven */

#ifndef JAOS_POLARIS_VERTICAL_FIXTURE_H
#define JAOS_POLARIS_VERTICAL_FIXTURE_H
#include "polaris_timing_actual_fixture.h"
#include "polaris_vertical_protocol.h"
/* Actual v37 first80 values plus SYNTHETIC min/max/control for CPU tests.
 * The final six values are not observed physical evidence. */
static inline void polaris_vertical_fixture(JcosU32 v[POLARIS_VERTICAL_VALUES]){
 polaris_timing_actual_fixture(v);v[73]=0x758FU;
 v[80]=v[81]=v[83]=v[84]=0x464U;v[82]=v[85]=0;
}
#endif

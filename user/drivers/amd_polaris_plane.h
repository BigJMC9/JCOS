#ifndef JCOS_AMD_POLARIS_PLANE_H
#define JCOS_AMD_POLARIS_PLANE_H
#include "amd_polaris_display.h"
#include "../../include/polaris_plane_protocol.h"
/* First programming policy: rebind only an already observed linear32 surface.
 * This function creates data. It cannot access hardware, reserve memory,
 * retire GOP, install an output descriptor or prove update completion. */
int polaris_plane_recipe_step(const PolarisDisplayObservation *observation,
    const PolarisPlaneWitness *witness, JcosU32 pipe, JcosU32 index,
    JcosU64 *out_tag, JcosU32 *out_value);
#endif

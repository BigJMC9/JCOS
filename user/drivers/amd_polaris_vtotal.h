#ifndef JAOS_AMD_POLARIS_VTOTAL_H
#define JAOS_AMD_POLARIS_VTOTAL_H
#include "../../include/polaris_vtotal_protocol.h"
typedef struct {void *context;int (*exchange)(void *,JcosU32,JcosU32,JcosU32,JcosU32 *);} PolarisUserVtotalIo;
int polaris_user_vtotal(const PolarisUserVtotalIo *io,int restore,JcosU32 *delta);
#endif

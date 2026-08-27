#ifndef JA_OS_TAR_H
#define JA_OS_TAR_H

#include "types.h"

bool tar_mount(
    const void *archive,
    u64 archive_size
);

#endif
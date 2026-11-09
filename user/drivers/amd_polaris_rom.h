#ifndef JCOS_AMD_POLARIS_ROM_H
#define JCOS_AMD_POLARIS_ROM_H
#include "../../include/polaris_rom_protocol.h"
typedef struct {
 JcosU32 status,crc32,image_bytes;
 unsigned short rom_header,master_data,object_header,object_bytes;
 unsigned char format,revision,count;
 JcosU64 connectors[POLARIS_ROM_CONNECTORS];
} PolarisRomInfo;
/* First uncompressed legacy PCI image only. Untrusted tables are data;
 * no command bytecode, register access, allocation, or firmware execution. */
void polaris_rom_parse(const unsigned char *bytes,JcosU32 size,JcosU32 vendor_device,PolarisRomInfo *out);
JcosU64 polaris_rom_query(const PolarisRomInfo *info,JcosU32 index);
#endif

#ifndef JCOS_AMD_POLARIS_ROM_H
#define JCOS_AMD_POLARIS_ROM_H
#include "../../include/polaris_rom_protocol.h"
typedef struct {
 JcosU32 status,flags,source_count,dest_count,record_count,other_count,record_types;
 unsigned short sources[POLARIS_ROM_LINK_IDS],destinations[POLARIS_ROM_LINK_IDS];
 unsigned char i2c_id,i2c_slave,hpd_gpio,hpd_state;
 unsigned short i2c_registers[8];
 unsigned char i2c_shifts[8];
 unsigned short hpd_register;
 unsigned char hpd_shift;
 JcosU64 table_info;
} PolarisRomRoute;
typedef struct {
 JcosU32 status,crc32,image_bytes;
 unsigned short rom_header,master_data,object_header,object_bytes;
 unsigned char format,revision,count;
 JcosU64 connectors[POLARIS_ROM_CONNECTORS];
 PolarisRomRoute routes[POLARIS_ROM_CONNECTORS];
 JcosU64 topology[POLARIS_ROM_CONNECTORS][POLARIS_ROM_TOPOLOGY_WORDS];
} PolarisRomInfo;
/* First uncompressed legacy PCI image only. Untrusted tables are data;
 * no command bytecode, register access, allocation, or firmware execution. */
void polaris_rom_parse(const unsigned char *bytes,JcosU32 size,JcosU32 vendor_device,PolarisRomInfo *out);
void polaris_rom_routes_parse(const unsigned char *bytes,JcosU32 image_bytes,PolarisRomInfo *info);
void polaris_rom_topology_parse(const unsigned char *bytes,JcosU32 image_bytes,PolarisRomInfo *info);
JcosU64 polaris_rom_query(const PolarisRomInfo *info,JcosU32 index);
/* Up to eight bytes from the process-owned RO copy;clears value on refusal. */
int polaris_rom_read_bytes(const unsigned char *bytes,JcosU32 size,JcosU64 offset,JcosU32 count,JcosU64 *value);
#endif

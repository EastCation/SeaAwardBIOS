#ifndef __AWARD_H
#define __AWARD_H

#include "types.h"

#define AWARD_BOOT_NONE     0
#define AWARD_BOOT_FLOPPY   1
#define AWARD_BOOT_HDD      2
#define AWARD_BOOT_CDROM    3
#define AWARD_BOOT_NETWORK  4

#define AWARD_SCSI_VIRTIO   0
#define AWARD_SCSI_LSI      1
#define AWARD_SCSI_ESP      2

void award_init(void);
void award_bios_setup(void);
int award_boot_device(int index);
int award_boot_other(void);
int award_network_boot_allowed(void);
int award_has_nvram(void);
int award_ide_device_enabled(int channel, int slave);
int award_ide_channel_enabled(int channel);
int award_usb_enabled(void);
int award_acpi_enabled(void);
int award_fdd_enabled(void);
int award_scsi_enabled(int type);
u16 award_serial_port(int index);
u16 award_parallel_port(void);

#endif

// Award-style CMOS setup utility with QEMU pflash NVRAM.
//
// Copyright (C) 2026  The SeaBIOS authors
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "award.h"
#include "block.h"
#include "biosvar.h"
#include "bregs.h"
#include "config.h"
#include "farptr.h"
#include "fw/paravirt.h"
#include "hw/ata.h"
#include "hw/esp-scsi.h"
#include "hw/lsi-scsi.h"
#include "hw/pci.h"
#include "hw/pci_regs.h"
#include "hw/pcidevice.h"
#include "hw/rtc.h"
#include "hw/virtio-scsi.h"
#include "malloc.h"
#include "output.h"
#include "stacks.h"
#include "std/bda.h"
#include "string.h"
#include "util.h"
#include "x86.h"
#include "award_logo.h"

#if CONFIG_AWARD_SETUP

#define AWARD_MAGIC         0x41574452
#define AWARD_VERSION       2
#define AWARD_NVRAM_ADDR    0xfff80000
#define AWARD_SECTOR_SIZE   4096
#define AWARD_MAX_ITEMS     16
#define AWARD_VAL_COL       32
#define IDE_ID_BASE         0x40
#define SCSI_ID_BASE        0x50

#define AI_ENUM     0
#define AI_INFO     1
#define AI_DATE     2
#define AI_TIME     3

#define INFO_BASE_MEM       1
#define INFO_EXT_MEM        2
#define INFO_TOTAL_MEM      3

#define ACT_LOAD_BIOS       1
#define ACT_LOAD_SETUP      2
#define ACT_IDE_DETECT      3
#define ACT_SAVE_EXIT       4
#define ACT_EXIT            5

#define ATTR_NORMAL         0x1f
#define ATTR_TITLE          0x1f
#define ATTR_VALUE          0x1e
#define ATTR_SELECT         0x60
#define ATTR_SELECT_VALUE   0x60
#define ATTR_BAR            0x70
#define ATTR_BOX            0x1f
#define ATTR_HELP           0x17
#define ATTR_SHADOW         0x00
#define ATTR_POST           0x07
#define ATTR_POST_TITLE     0x0f
#define ATTR_POST_PROMPT    0x0e

struct award_nvram {
    u8 boot_order[3];
    u8 boot_other;
    u8 boot_numlock;
    u8 floppy_a;
    u8 floppy_b;
    u8 ide[4];
    u8 onboard_ide1;
    u8 onboard_ide2;
    u8 usb_ctrl;
    u8 virtio_scsi;
    u8 lsi_scsi;
    u8 esp_scsi;
    u8 serial1;
    u8 serial2;
    u8 parallel;
    u8 fdd_ctrl;
    u8 acpi;
    u8 century;
    u8 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u8 reserved[1];
} __attribute__((packed));

struct award_record {
    u32 magic;
    u16 version;
    u16 size;
    u32 seq;
    u32 checksum;
    struct award_nvram cfg;
} __attribute__((packed));

struct award_item {
    const char *name;
    const char *help;
    u8 type;
    u8 id;
    u8 *value;
    const char *const *choices;
    u8 nchoices;
};

struct award_page {
    const char *title;
    const struct award_item *items;
    u8 nitems;
};

struct award_main_item {
    const char *name;
    const struct award_page *page;
    u8 action;
};

static struct award_nvram AwardCfg;
static u32 AwardSeq;
static int AwardSlot = -1;
static int AwardLoaded;
static int AwardFlash = -1;
static int AwardDateTouched;
static int AwardTimeTouched;
static u32 AwardCpuMhz;
static int AwardCpuMhzValid;

static void award_sanitize(void);
static void award_ide_post_status(int index, char *buf, int size);
static void award_scsi_status(int type, char *buf, int size);
static int award_post_wait(int msec, int allow_setup);

static u8
award_bcd2bin(u8 val)
{
    return (val & 0xf) + ((val >> 4) * 10);
}

static u8
award_bin2bcd(u8 val)
{
    return ((val / 10) << 4) | (val % 10);
}

static u8
award_flash_read(u32 off)
{
    return *(volatile u8 *)(AWARD_NVRAM_ADDR + off);
}

static void
award_flash_write(u32 off, u8 val)
{
    *(volatile u8 *)(AWARD_NVRAM_ADDR + off) = val;
    barrier();
}

static int
award_flash_present(void)
{
    if (AwardFlash >= 0)
        return AwardFlash;
    award_flash_write(0x10, 0x98);
    int ok = (award_flash_read(0x10) == 'Q'
              && award_flash_read(0x11) == 'R'
              && award_flash_read(0x12) == 'Y');
    award_flash_write(0, 0xff);
    AwardFlash = ok;
    return ok;
}

static void
award_flash_erase(int slot)
{
    u32 off = slot * AWARD_SECTOR_SIZE;
    award_flash_write(off, 0x20);
    award_flash_write(off, 0xff);
}

static void
award_flash_program(u32 off, const void *src, u32 len)
{
    const u8 *p = src;
    u32 i;
    for (i = 0; i < len; i++) {
        award_flash_write(0, 0x10);
        award_flash_write(off + i, p[i]);
    }
    award_flash_write(0, 0xff);
}

static int
award_read_slot(int slot, struct award_record *rec)
{
    u32 off = slot * AWARD_SECTOR_SIZE;
    memcpy(rec, (void *)(AWARD_NVRAM_ADDR + off), sizeof(*rec));
    if (rec->magic != AWARD_MAGIC || rec->version != AWARD_VERSION
        || rec->size != sizeof(AwardCfg))
        return 0;
    if (checksum(rec, sizeof(*rec)) != 0)
        return 0;
    return 1;
}

static void
award_defaults(struct award_nvram *c)
{
    memset(c, 0, sizeof(*c));
    c->boot_order[0] = AWARD_BOOT_HDD;
    c->boot_order[1] = AWARD_BOOT_CDROM;
    c->boot_order[2] = AWARD_BOOT_FLOPPY;
    c->boot_other = 1;
    c->boot_numlock = 1;
    u8 ft = rtc_read(CMOS_FLOPPY_DRIVE_TYPE);
    c->floppy_a = ft >> 4;
    c->floppy_b = ft & 0xf;
    if (c->floppy_a > 5)
        c->floppy_a = 0;
    if (c->floppy_b > 5)
        c->floppy_b = 0;
    c->ide[0] = 0;
    c->ide[1] = 0;
    c->ide[2] = 0;
    c->ide[3] = 0;
    c->onboard_ide1 = 1;
    c->onboard_ide2 = 1;
    c->usb_ctrl = 1;
    c->virtio_scsi = 1;
    c->lsi_scsi = 1;
    c->esp_scsi = 1;
    c->serial1 = 0;
    c->serial2 = 1;
    c->parallel = 0;
    c->fdd_ctrl = 1;
    c->acpi = 1;
    c->century = 20;
    c->year = 26;
    c->month = 1;
    c->day = 1;
}

static void
award_load_failsafe(struct award_nvram *c)
{
    struct award_nvram old = *c;
    award_defaults(c);
    c->boot_order[0] = AWARD_BOOT_FLOPPY;
    c->boot_order[1] = AWARD_BOOT_HDD;
    c->boot_order[2] = AWARD_BOOT_CDROM;
    c->boot_numlock = 0;
    c->century = old.century;
    c->year = old.year;
    c->month = old.month;
    c->day = old.day;
    c->hour = old.hour;
    c->minute = old.minute;
    c->second = old.second;
}

static void
award_load_setup(struct award_nvram *c)
{
    struct award_nvram old = *c;
    award_defaults(c);
    c->century = old.century;
    c->year = old.year;
    c->month = old.month;
    c->day = old.day;
    c->hour = old.hour;
    c->minute = old.minute;
    c->second = old.second;
}

static void
award_apply_floppy(void)
{
    u8 type = 0;
    if (AwardCfg.fdd_ctrl)
        type = (AwardCfg.floppy_a << 4) | AwardCfg.floppy_b;
    rtc_write(CMOS_FLOPPY_DRIVE_TYPE, type);
}

void
award_init(void)
{
    if (AwardLoaded)
        return;
    AwardLoaded = 1;
    award_defaults(&AwardCfg);
    if (award_flash_present()) {
        struct award_record r0, r1;
        int ok0 = award_read_slot(0, &r0);
        int ok1 = award_read_slot(1, &r1);
        if (ok0 && (!ok1 || (s32)(r0.seq - r1.seq) >= 0)) {
            AwardCfg = r0.cfg;
            AwardSeq = r0.seq;
            AwardSlot = 0;
        } else if (ok1) {
            AwardCfg = r1.cfg;
            AwardSeq = r1.seq;
            AwardSlot = 1;
        }
    }
    award_sanitize();
    award_apply_floppy();
}

int
award_has_nvram(void)
{
    award_init();
    return AwardSlot >= 0;
}

int
award_boot_device(int index)
{
    award_init();
    if (index < 0 || index >= 3)
        return AWARD_BOOT_NONE;
    return AwardCfg.boot_order[index];
}

int
award_boot_other(void)
{
    award_init();
    return AwardCfg.boot_other;
}

int
award_network_boot_allowed(void)
{
    award_init();
    if (AwardCfg.boot_other)
        return 1;
    int i;
    for (i = 0; i < 3; i++)
        if (AwardCfg.boot_order[i] == AWARD_BOOT_NETWORK)
            return 1;
    return 0;
}

int
award_ide_device_enabled(int channel, int slave)
{
    award_init();
    if (channel < 0 || channel > 1 || slave < 0 || slave > 1)
        return 1;
    return AwardCfg.ide[channel * 2 + slave] == 0;
}

int
award_ide_channel_enabled(int channel)
{
    award_init();
    if (channel == 0)
        return AwardCfg.onboard_ide1;
    if (channel == 1)
        return AwardCfg.onboard_ide2;
    return 1;
}

int
award_usb_enabled(void)
{
    award_init();
    return AwardCfg.usb_ctrl;
}

int
award_acpi_enabled(void)
{
    award_init();
    return AwardCfg.acpi;
}

int
award_fdd_enabled(void)
{
    award_init();
    return AwardCfg.fdd_ctrl;
}

int
award_scsi_enabled(int type)
{
    award_init();
    switch (type) {
    case AWARD_SCSI_VIRTIO:
        return AwardCfg.virtio_scsi;
    case AWARD_SCSI_LSI:
        return AwardCfg.lsi_scsi;
    case AWARD_SCSI_ESP:
        return AwardCfg.esp_scsi;
    }
    return 1;
}

u16
award_serial_port(int index)
{
    static const u16 ports[] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8, 0 };
    award_init();
    u8 val = index ? AwardCfg.serial2 : AwardCfg.serial1;
    if (val >= ARRAY_SIZE(ports))
        val = ARRAY_SIZE(ports) - 1;
    return ports[val];
}

u16
award_parallel_port(void)
{
    static const u16 ports[] = { 0x378, 0x278, 0x3bc, 0 };
    award_init();
    u8 val = AwardCfg.parallel;
    if (val >= ARRAY_SIZE(ports))
        val = ARRAY_SIZE(ports) - 1;
    return ports[val];
}

static int
award_save(void)
{
    if (!award_flash_present())
        return 0;
    struct award_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = AWARD_MAGIC;
    rec.version = AWARD_VERSION;
    rec.size = sizeof(AwardCfg);
    rec.seq = AwardSeq + 1;
    rec.cfg = AwardCfg;
    u8 sum = checksum(&rec, sizeof(rec));
    rec.checksum = (u8)(0 - sum);
    int slot = (AwardSlot == 0) ? 1 : 0;
    u32 off = slot * AWARD_SECTOR_SIZE;
    award_flash_erase(slot);
    award_flash_program(off, &rec, sizeof(rec));
    struct award_record chk;
    if (!award_read_slot(slot, &chk) || chk.seq != rec.seq)
        return 0;
    AwardSlot = slot;
    AwardSeq = rec.seq;
    return 1;
}

static void
award_rtc_read_time(struct award_nvram *c)
{
    if (rtc_updating())
        return;
    c->second = award_bcd2bin(rtc_read(CMOS_RTC_SECONDS));
    c->minute = award_bcd2bin(rtc_read(CMOS_RTC_MINUTES));
    c->hour = award_bcd2bin(rtc_read(CMOS_RTC_HOURS));
}

static void
award_rtc_read_date(struct award_nvram *c)
{
    if (rtc_updating())
        return;
    c->year = award_bcd2bin(rtc_read(CMOS_RTC_YEAR));
    c->month = award_bcd2bin(rtc_read(CMOS_RTC_MONTH));
    c->day = award_bcd2bin(rtc_read(CMOS_RTC_DAY_MONTH));
    c->century = award_bcd2bin(rtc_read(CMOS_CENTURY));
}

static void
award_rtc_write(void)
{
    if (rtc_updating())
        return;
    u8 status = rtc_read(CMOS_STATUS_B);
    rtc_write(CMOS_STATUS_B, status | RTC_B_SET);
    rtc_write(CMOS_RTC_SECONDS, award_bin2bcd(AwardCfg.second));
    rtc_write(CMOS_RTC_MINUTES, award_bin2bcd(AwardCfg.minute));
    rtc_write(CMOS_RTC_HOURS, award_bin2bcd(AwardCfg.hour));
    rtc_write(CMOS_RTC_YEAR, award_bin2bcd(AwardCfg.year));
    rtc_write(CMOS_RTC_MONTH, award_bin2bcd(AwardCfg.month));
    rtc_write(CMOS_RTC_DAY_MONTH, award_bin2bcd(AwardCfg.day));
    rtc_write(CMOS_CENTURY, award_bin2bcd(AwardCfg.century));
    rtc_write(CMOS_STATUS_B, (status & ~RTC_B_SET) | RTC_B_24HR);
}

static void
award_numlock(void)
{
    u16 flags = GET_BDA(kbd_flag0);
    if (AwardCfg.boot_numlock)
        flags |= KF0_NUM | KF0_NUMACTIVE;
    else
        flags &= ~(KF0_NUM | KF0_NUMACTIVE);
    SET_BDA(kbd_flag0, flags);
}

static struct drive_s *
award_ide_find(int channel, int slave, const char **desc)
{
    int i;
    for (i = 0; ; i++) {
        const char *dsc = NULL;
        struct drive_s *d = boot_drive_at(i, &dsc);
        if (!d)
            break;
        u8 type = GET_GLOBALFLAT(d->type);
        if (type != DTYPE_ATA && type != DTYPE_ATA_ATAPI)
            continue;
        struct atadrive_s *ad = container_of(d, struct atadrive_s, drive);
        struct ata_channel_s *ch = GET_GLOBALFLAT(ad->chan_gf);
        if (GET_GLOBALFLAT(ch->chanid) == channel
            && GET_GLOBALFLAT(ad->slave) == slave) {
            if (desc)
                *desc = dsc;
            return d;
        }
    }
    return NULL;
}

static void
award_ide_status(int index, char *buf, int size)
{
    const char *desc = NULL;
    struct drive_s *d = award_ide_find(index >> 1, index & 1, &desc);
    if (!d) {
        snprintf(buf, size, "Not detected");
        return;
    }
    if (desc && *desc) {
        strtcpy(buf, desc, size);
        return;
    }
    u8 type = GET_GLOBALFLAT(d->type);
    if (type == DTYPE_ATA_ATAPI) {
        snprintf(buf, size, "ATAPI device");
        return;
    }
    u32 sectors = (u32)GET_GLOBALFLAT(d->sectors);
    snprintf(buf, size, "ATA HDD, %u MB", sectors / 2048);
}

static void
award_vga_put(int row, int col, u8 ch, u8 attr)
{
    if (row < 0 || row >= 25 || col < 0 || col >= 80)
        return;
    ((volatile u16 *)0xb8000)[row * 80 + col] = (attr << 8) | ch;
}

static void
award_vga_fill(int row, int col, int width, u8 attr)
{
    int i;
    for (i = 0; i < width; i++)
        award_vga_put(row, col + i, ' ', attr);
}

static void
award_vga_write(int row, int col, const char *s, u8 attr)
{
    while (*s) {
        award_vga_put(row, col++, *s, attr);
        s++;
    }
}

static void
award_vga_center(int row, const char *s, u8 attr)
{
    award_vga_write(row, (80 - strlen(s)) / 2, s, attr);
}

static void
award_vga_box(int x, int y, int w, int h, u8 attr)
{
    int i;
    award_vga_put(y, x, 0xc9, attr);
    award_vga_put(y, x + w - 1, 0xbb, attr);
    award_vga_put(y + h - 1, x, 0xc8, attr);
    award_vga_put(y + h - 1, x + w - 1, 0xbc, attr);
    for (i = 1; i < w - 1; i++) {
        award_vga_put(y, x + i, 0xcd, attr);
        award_vga_put(y + h - 1, x + i, 0xcd, attr);
    }
    for (i = 1; i < h - 1; i++) {
        award_vga_put(y + i, x, 0xba, attr);
        award_vga_put(y + i, x + w - 1, 0xba, attr);
    }
}

static void
award_vga_mode(void)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.ax = 0x0003;
    br.flags = F_IF;
    call16_int(0x10, &br);
    memset(&br, 0, sizeof(br));
    br.ax = 0x0100;
    br.cx = 0x2000;
    br.flags = F_IF;
    call16_int(0x10, &br);
}

static u8 *AwardLogoFontLow;
static u8 AwardLogoFontSaved[128 * 16];

static int
award_logo_font_load(void)
{
    struct bregs br;
    AwardLogoFontLow = malloc_tmplow(256 * 16);
    if (!AwardLogoFontLow) {
        warn_noalloc();
        return -1;
    }
    memset(&br, 0, sizeof(br));
    br.ax = 0x1130;
    br.bh = 0x06;
    br.flags = F_IF;
    call16_int(0x10, &br);
    memcpy(AwardLogoFontLow, MAKE_FLATPTR(br.es, br.bp), 256 * 16);
    memcpy(AwardLogoFontSaved, AwardLogoFontLow + 128 * 16,
           sizeof(AwardLogoFontSaved));
    memcpy(AwardLogoFontLow + 128 * 16, AwardLogoFont,
           sizeof(AwardLogoFont));
    memset(&br, 0, sizeof(br));
    br.ax = 0x1100;
    br.bx = 0x1000;
    br.cx = 256;
    br.es = FLATPTR_TO_SEG(AwardLogoFontLow);
    br.bp = FLATPTR_TO_OFFSET(AwardLogoFontLow);
    br.flags = F_IF;
    call16_int(0x10, &br);
    return 0;
}

static void
award_logo_font_restore(void)
{
    struct bregs br;
    if (!AwardLogoFontLow)
        return;
    memcpy(AwardLogoFontLow + 128 * 16, AwardLogoFontSaved,
           sizeof(AwardLogoFontSaved));
    memset(&br, 0, sizeof(br));
    br.ax = 0x1100;
    br.bx = 0x1000;
    br.cx = 256;
    br.es = FLATPTR_TO_SEG(AwardLogoFontLow);
    br.bp = FLATPTR_TO_OFFSET(AwardLogoFontLow);
    br.flags = F_IF;
    call16_int(0x10, &br);
    free(AwardLogoFontLow);
    AwardLogoFontLow = NULL;
}

static void
award_logo_draw(void)
{
    int row, col;
    if (award_logo_font_load())
        return;
    for (row = 0; row < AWARD_LOGO_ROWS; row++) {
        for (col = 0; col < AWARD_LOGO_COLS; col++) {
            int idx = row * AWARD_LOGO_COLS + col;
            u8 kind = AwardLogoColor[idx];
            u8 ch = kind ? AWARD_LOGO_BASE + idx : ' ';
            u8 attr = kind == 1 ? 0x0e : kind == 2 ? 0x0a : ATTR_POST;
            award_vga_put(row, 80 - AWARD_LOGO_COLS + col, ch, attr);
        }
    }
}

static void
award_logo_clear(void)
{
    int row;
    for (row = 0; row < AWARD_LOGO_ROWS; row++)
        award_vga_fill(row, 80 - AWARD_LOGO_COLS, AWARD_LOGO_COLS,
                       ATTR_POST);
}

static const char *const AwardMonths[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const ChoiceEnabled[] = { "Disabled", "Enabled" };
static const char *const ChoiceOnOff[] = { "Off", "On" };
static const char *const ChoiceBootDev[] = {
    "Disabled", "Floppy", "Hard Disk", "CDROM", "Network"
};
static const char *const ChoiceIde[] = { "Auto", "Disabled" };
static const char *const ChoiceFloppy[] = {
    "None", "360K, 5.25 in", "1.2M, 5.25 in",
    "720K, 3.5 in", "1.44M, 3.5 in", "2.88M, 3.5 in"
};
static const char *const ChoiceSerial[] = {
    "3F8/IRQ4", "2F8/IRQ3", "3E8/IRQ4", "2E8/IRQ3", "Disabled"
};
static const char *const ChoiceParallel[] = {
    "378/IRQ7", "278/IRQ5", "3BC/IRQ7", "Disabled"
};

#define IT_ENUM(n, h, v, c) { n, h, AI_ENUM, 0, v, c, ARRAY_SIZE(c) }
#define IT_IDE(n, h, v, i) { n, h, AI_ENUM, i, v, ChoiceIde, ARRAY_SIZE(ChoiceIde) }
#define IT_SCSI(n, h, v, i) { n, h, AI_ENUM, i, v, ChoiceEnabled, ARRAY_SIZE(ChoiceEnabled) }
#define IT_INFO(n, h, i)    { n, h, AI_INFO, i, 0, 0, 0 }
#define IT_DATE(n, h)       { n, h, AI_DATE, 0, 0, 0, 0 }
#define IT_TIME(n, h)       { n, h, AI_TIME, 1, 0, 0, 0 }

static const struct award_item StandardItems[] = {
    IT_DATE("Date (mm:dd:yy)", "Set the system date"),
    IT_TIME("Time (hh:mm:ss)", "Set the system time"),
    IT_IDE("IDE Primary Master", "Primary master IDE device",
           &AwardCfg.ide[0], IDE_ID_BASE + 0),
    IT_IDE("IDE Primary Slave", "Primary slave IDE device",
           &AwardCfg.ide[1], IDE_ID_BASE + 1),
    IT_IDE("IDE Secondary Master", "Secondary master IDE device",
           &AwardCfg.ide[2], IDE_ID_BASE + 2),
    IT_IDE("IDE Secondary Slave", "Secondary slave IDE device",
           &AwardCfg.ide[3], IDE_ID_BASE + 3),
    IT_ENUM("Drive A", "Select the floppy drive type",
            &AwardCfg.floppy_a, ChoiceFloppy),
    IT_ENUM("Drive B", "Select the floppy drive type",
            &AwardCfg.floppy_b, ChoiceFloppy),
    IT_INFO("Base Memory", "Base memory detected during POST", INFO_BASE_MEM),
    IT_INFO("Extended Memory", "Extended memory detected during POST",
            INFO_EXT_MEM),
    IT_INFO("Total Memory", "Total memory detected during POST", INFO_TOTAL_MEM),
};

static const struct award_item BiosItems[] = {
    IT_ENUM("First Boot Device", "Select the first boot device",
            &AwardCfg.boot_order[0], ChoiceBootDev),
    IT_ENUM("Second Boot Device", "Select the second boot device",
            &AwardCfg.boot_order[1], ChoiceBootDev),
    IT_ENUM("Third Boot Device", "Select the third boot device",
            &AwardCfg.boot_order[2], ChoiceBootDev),
    IT_ENUM("Boot Other Device", "Try other boot devices",
            &AwardCfg.boot_other, ChoiceEnabled),
    IT_ENUM("Boot Up NumLock Status", "Set NumLock state at boot",
            &AwardCfg.boot_numlock, ChoiceOnOff),
};

static const struct award_item PeriphItems[] = {
    IT_ENUM("On-Chip Primary PCI IDE", "Enable the primary IDE channel",
            &AwardCfg.onboard_ide1, ChoiceEnabled),
    IT_ENUM("On-Chip Secondary PCI IDE", "Enable the secondary IDE channel",
            &AwardCfg.onboard_ide2, ChoiceEnabled),
    IT_ENUM("USB Controller", "Enable the USB controller",
            &AwardCfg.usb_ctrl, ChoiceEnabled),
    IT_ENUM("Onboard Serial Port 1", "Select the first serial port",
            &AwardCfg.serial1, ChoiceSerial),
    IT_ENUM("Onboard Serial Port 2", "Select the second serial port",
            &AwardCfg.serial2, ChoiceSerial),
    IT_ENUM("Onboard Parallel Port", "Select the parallel port",
            &AwardCfg.parallel, ChoiceParallel),
    IT_ENUM("Onboard FDD Controller", "Enable the floppy controller",
            &AwardCfg.fdd_ctrl, ChoiceEnabled),
};

static const struct award_item ScsiItems[] = {
    IT_SCSI("VirtIO SCSI BIOS", "Enable the virtio-scsi BIOS",
            &AwardCfg.virtio_scsi, SCSI_ID_BASE + 0),
    IT_SCSI("LSI 53C895A SCSI BIOS", "Enable the LSI 53C895A SCSI BIOS",
            &AwardCfg.lsi_scsi, SCSI_ID_BASE + 1),
    IT_SCSI("AMD 53C974A SCSI BIOS", "Enable the AMD 53C974A SCSI BIOS",
            &AwardCfg.esp_scsi, SCSI_ID_BASE + 2),
};

static const struct award_item PowerItems[] = {
    IT_ENUM("ACPI Function", "Enable ACPI support",
            &AwardCfg.acpi, ChoiceEnabled),
};

static const struct award_page AwardPages[] = {
    { "Standard CMOS Features", StandardItems, ARRAY_SIZE(StandardItems) },
    { "Advanced BIOS Features", BiosItems, ARRAY_SIZE(BiosItems) },
    { "Integrated Peripherals", PeriphItems, ARRAY_SIZE(PeriphItems) },
    { "SCSI BIOS Configuration", ScsiItems, ARRAY_SIZE(ScsiItems) },
    { "Power Management Setup", PowerItems, ARRAY_SIZE(PowerItems) },
};

static const struct award_main_item MenuLeft[] = {
    { "Standard CMOS Features", &AwardPages[0], 0 },
    { "Advanced BIOS Features", &AwardPages[1], 0 },
    { "Integrated Peripherals", &AwardPages[2], 0 },
    { "SCSI BIOS Configuration", &AwardPages[3], 0 },
    { "Power Management Setup", &AwardPages[4], 0 },
};

static const struct award_main_item MenuRight[] = {
    { "Load Fail-Safe Defaults", 0, ACT_LOAD_BIOS },
    { "Load Optimized Defaults", 0, ACT_LOAD_SETUP },
    { "IDE HDD Auto Detection", 0, ACT_IDE_DETECT },
    { "Save & Exit Setup", 0, ACT_SAVE_EXIT },
    { "Exit Without Saving", 0, ACT_EXIT },
};

static void
award_sanitize(void)
{
    int p, i;
    for (p = 0; p < ARRAY_SIZE(AwardPages); p++) {
        for (i = 0; i < AwardPages[p].nitems; i++) {
            const struct award_item *it = &AwardPages[p].items[i];
            if (it->type == AI_ENUM && it->value
                && *it->value >= it->nchoices)
                *it->value = 0;
        }
    }
    for (i = 0; i < 3; i++)
        if (AwardCfg.boot_order[i] > AWARD_BOOT_NETWORK)
            AwardCfg.boot_order[i] = AWARD_BOOT_NONE;
    if (AwardCfg.floppy_a > 5)
        AwardCfg.floppy_a = 0;
    if (AwardCfg.floppy_b > 5)
        AwardCfg.floppy_b = 0;
    if (AwardCfg.serial1 > 4)
        AwardCfg.serial1 = 4;
    if (AwardCfg.serial2 > 4)
        AwardCfg.serial2 = 4;
    if (AwardCfg.parallel > 3)
        AwardCfg.parallel = 3;
    if (AwardCfg.month < 1 || AwardCfg.month > 12)
        AwardCfg.month = 1;
    if (AwardCfg.day < 1 || AwardCfg.day > 31)
        AwardCfg.day = 1;
    if (AwardCfg.hour > 23)
        AwardCfg.hour = 0;
    if (AwardCfg.minute > 59)
        AwardCfg.minute = 0;
    if (AwardCfg.second > 59)
        AwardCfg.second = 0;
}

static void
award_draw_frame(const char *title, int footer_sep)
{
    int r, c;
    for (r = 1; r < 24; r++)
        award_vga_fill(r, 1, 78, ATTR_NORMAL);
    award_vga_put(0, 0, 0xc9, ATTR_BOX);
    award_vga_put(0, 79, 0xbb, ATTR_BOX);
    for (c = 1; c < 79; c++)
        award_vga_put(0, c, 0xcd, ATTR_BOX);
    award_vga_put(24, 0, 0xc8, ATTR_BOX);
    award_vga_put(24, 79, 0xbc, ATTR_BOX);
    for (c = 1; c < 79; c++)
        award_vga_put(24, c, 0xcd, ATTR_BOX);
    for (r = 1; r < 24; r++) {
        award_vga_put(r, 0, 0xba, ATTR_BOX);
        award_vga_put(r, 79, 0xba, ATTR_BOX);
    }
    award_vga_put(2, 0, 0xcc, ATTR_BOX);
    award_vga_put(2, 79, 0xb9, ATTR_BOX);
    for (c = 1; c < 79; c++)
        award_vga_put(2, c, 0xcd, ATTR_BOX);
    award_vga_center(1, title, ATTR_TITLE);
    if (footer_sep) {
        award_vga_put(21, 0, 0xcc, ATTR_BOX);
        award_vga_put(21, 79, 0xb9, ATTR_BOX);
        for (c = 1; c < 79; c++)
            award_vga_put(21, c, 0xcd, ATTR_BOX);
    }
}

static void
award_draw_main_item(const char *name, int bar_x, int text_x, int row,
                     int selected)
{
    if (selected) {
        award_vga_fill(row, bar_x, 37, ATTR_SELECT);
        award_vga_write(row, text_x, name, ATTR_SELECT);
    } else {
        award_vga_write(row, text_x, name, ATTR_NORMAL);
    }
}

static void
award_two_digits(char *buf, u8 val)
{
    buf[0] = '0' + (val / 10) % 10;
    buf[1] = '0' + val % 10;
    buf[2] = 0;
}

static void
award_format_date(char *buf, int size)
{
    int month = AwardCfg.month;
    if (month < 1 || month > 12)
        month = 1;
    char mon[3], day[3], century[3], year[3];
    award_two_digits(mon, month);
    award_two_digits(day, AwardCfg.day);
    award_two_digits(century, AwardCfg.century);
    award_two_digits(year, AwardCfg.year);
    snprintf(buf, size, "%s/%s/%s%s", mon, day, century, year);
}

static void
award_format_time(char *buf, int size)
{
    char hour[3], minute[3], second[3];
    award_two_digits(hour, AwardCfg.hour);
    award_two_digits(minute, AwardCfg.minute);
    award_two_digits(second, AwardCfg.second);
    snprintf(buf, size, "%s:%s:%s", hour, minute, second);
}

static void
award_draw_value(int row, const struct award_item *it, int selected)
{
    char buf[64];
    u8 attr = selected ? ATTR_SELECT_VALUE : ATTR_VALUE;
    if (it->type == AI_ENUM) {
        u8 val = *it->value;
        if (val >= it->nchoices)
            val = 0;
        if (it->id >= IDE_ID_BASE && it->id < IDE_ID_BASE + 4) {
            if (val) {
                snprintf(buf, sizeof(buf), "[None]");
            } else {
                char status[48];
                award_ide_post_status(it->id - IDE_ID_BASE, status,
                                      sizeof(status));
                snprintf(buf, sizeof(buf), "[%s]", status);
            }
        } else {
            snprintf(buf, sizeof(buf), "[%s]", it->choices[val]);
        }
        award_vga_write(row, AWARD_VAL_COL, buf, attr);
    } else if (it->type == AI_INFO) {
        u32 mem = RamSize / 1024;
        if (it->id == INFO_BASE_MEM)
            snprintf(buf, sizeof(buf), "%uK", 640);
        else if (it->id == INFO_EXT_MEM) {
            u32 ext = mem > 1024 ? mem - 1024 : 0;
            snprintf(buf, sizeof(buf), "%uK", ext);
        } else {
            snprintf(buf, sizeof(buf), "%uK", mem);
        }
        award_vga_write(row, AWARD_VAL_COL, buf, attr);
    } else if (it->type == AI_DATE) {
        award_format_date(buf, sizeof(buf));
        award_vga_write(row, AWARD_VAL_COL, buf, attr);
    } else if (it->type == AI_TIME) {
        award_format_time(buf, sizeof(buf));
        award_vga_write(row, AWARD_VAL_COL, buf, attr);
    }
}

static void
award_draw_main(int col, int row)
{
    int i, r;
    award_draw_frame("CMOS SETUP UTILITY - Copyright (C) 2020-2026, MoeRain Software Developments", 0);
    for (r = 3; r <= 15; r++)
        award_vga_fill(r, 1, 78, ATTR_NORMAL);
    for (i = 0; i < ARRAY_SIZE(MenuLeft); i++)
        award_draw_main_item(MenuLeft[i].name, 2, 3, 4 + i,
                             col == 0 && row == i);
    for (i = 0; i < ARRAY_SIZE(MenuRight); i++)
        award_draw_main_item(MenuRight[i].name, 41, 42, 4 + i,
                             col == 1 && row == i);
    award_vga_put(12, 0, 0xcc, ATTR_BOX);
    award_vga_put(12, 79, 0xb9, ATTR_BOX);
    for (r = 1; r < 79; r++)
        award_vga_put(12, r, 0xcd, ATTR_BOX);
    award_vga_write(13, 2, "Esc : Quit", ATTR_NORMAL);
    award_vga_write(13, 30, "\x18\x19\x1b\x1a : Select Item", ATTR_NORMAL);
    award_vga_write(15, 2, "F10 : Save & Exit Setup", ATTR_NORMAL);
    if (award_flash_present())
        award_vga_write(23, 2, "NVRAM : QEMU pflash (persistent)",
                        ATTR_HELP);
    else
        award_vga_write(23, 2,
                        "NVRAM : not detected (settings will not persist)",
                        ATTR_HELP);
}

static void
award_draw_page(const struct award_page *p, int sel)
{
    int i, r;
    award_draw_frame(p->title, 1);
    for (r = 3; r <= 20; r++)
        award_vga_fill(r, 1, 78, ATTR_NORMAL);
    for (i = 0; i < p->nitems && i < AWARD_MAX_ITEMS; i++) {
        const struct award_item *it = &p->items[i];
        int row = 4 + i;
        u8 attr = (i == sel) ? ATTR_SELECT : ATTR_NORMAL;
        if (i == sel)
            award_vga_fill(row, 2, 76, ATTR_SELECT);
        award_vga_write(row, 3, it->name, attr);
        award_vga_put(row, 30, ':', attr);
        award_draw_value(row, it, i == sel);
    }
    award_vga_write(22, 2,
                    "Esc:Return  \x18\x19:Select  Enter:Select  +/-:Change  F10:Save&Exit",
                    ATTR_NORMAL);
    if (sel >= 0 && sel < p->nitems) {
        const struct award_item *it = &p->items[sel];
        if (it->id >= IDE_ID_BASE && it->id < IDE_ID_BASE + 4) {
            char status[64];
            award_ide_status(it->id - IDE_ID_BASE, status, sizeof(status));
            award_vga_write(23, 2, status, ATTR_HELP);
        } else if (it->id >= SCSI_ID_BASE && it->id < SCSI_ID_BASE + 3) {
            char status[80];
            award_scsi_status(it->id - SCSI_ID_BASE, status, sizeof(status));
            snprintf(status + strlen(status), sizeof(status) - strlen(status),
                     "  (Enter: SCSI ID list)");
            award_vga_write(23, 2, status, ATTR_HELP);
        } else {
            award_vga_write(23, 2, it->help, ATTR_HELP);
        }
    }
}

static void
award_draw_shadow(int x, int y, int w, int h)
{
    int i;
    for (i = 1; i <= h; i++)
        award_vga_fill(y + i, x + 1, w, ATTR_SHADOW);
}

static void
award_popup_title(int x, int w, int row, const char *s, u8 attr)
{
    award_vga_write(row, x + (w - strlen(s)) / 2, s, attr);
}

static int
award_popup(const char *title, const char *const *choices, int n, int current,
            int anchor)
{
    if (n <= 0)
        return -1;
    int i, w = strlen(title) + 4;
    for (i = 0; i < n; i++) {
        int len = strlen(choices[i]) + 4;
        if (len > w)
            w = len;
    }
    if (w > 76)
        w = 76;
    int h = n + 4;
    if (h > 20)
        h = 20;
    int x = (80 - w) / 2;
    int y = anchor + 1;
    if (y + h > 23)
        y = 23 - h;
    if (y < 5)
        y = 5;
    if (current < 0 || current >= n)
        current = 0;
    for (;;) {
        award_draw_shadow(x, y, w, h);
        for (i = 1; i < h - 1; i++)
            award_vga_fill(y + i, x + 1, w - 2, ATTR_NORMAL);
        award_vga_box(x, y, w, h, ATTR_BOX);
        award_popup_title(x, w, y, title, ATTR_TITLE);
        for (i = 0; i < n && i < h - 4; i++) {
            int row = y + 2 + i;
            if (i == current) {
                award_vga_fill(row, x + 1, w - 2, ATTR_SELECT);
                award_popup_title(x, w, row, choices[i], ATTR_SELECT);
            } else {
                award_popup_title(x, w, row, choices[i], ATTR_NORMAL);
            }
        }
        int k = get_keystroke_full(100);
        if (k < 0)
            continue;
        int scan = k >> 8;
        if (scan == 0x01)
            return -1;
        if (scan == 0x1c)
            return current;
        if (scan == 0x48) {
            if (current > 0)
                current--;
            else
                current = n - 1;
        } else if (scan == 0x50) {
            if (current + 1 < n)
                current++;
            else
                current = 0;
        } else if (scan == 0x49) {
            current -= 4;
            if (current < 0)
                current = 0;
        } else if (scan == 0x51) {
            current += 4;
            if (current >= n)
                current = n - 1;
        } else if (scan == 0x47) {
            current = 0;
        } else if (scan == 0x4f) {
            current = n - 1;
        }
    }
}

static int
award_confirm(const char *msg)
{
    int w = strlen(msg) + 8;
    if (w < 34)
        w = 34;
    if (w > 76)
        w = 76;
    int h = 6;
    int x = (80 - w) / 2;
    int y = 9;
    int r;
    award_draw_shadow(x, y, w, h);
    for (r = 1; r < h - 1; r++)
        award_vga_fill(y + r, x + 1, w - 2, ATTR_NORMAL);
    award_vga_box(x, y, w, h, ATTR_BOX);
    award_popup_title(x, w, y + 2, msg, ATTR_VALUE);
    award_popup_title(x, w, y + 3, "(Y/N)", ATTR_NORMAL);
    for (;;) {
        int k = get_keystroke_full(100);
        if (k < 0)
            continue;
        int scan = k >> 8;
        int ascii = k & 0xff;
        if (scan == 0x01 || ascii == 'n' || ascii == 'N')
            return 0;
        if (scan == 0x1c || ascii == 'y' || ascii == 'Y')
            return 1;
    }
}

static int
award_days_in_month(int month, int year)
{
    static const u8 days[] = { 31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 31;
    if (month == 2 && (year % 4) == 0)
        return 29;
    return days[month - 1];
}

static int
award_edit_clock(int date)
{
    u8 v0 = date ? AwardCfg.month : AwardCfg.hour;
    u8 v1 = date ? AwardCfg.day : AwardCfg.minute;
    u8 v2 = date ? AwardCfg.year : AwardCfg.second;
    int field = 0, changed = 0;
    int w = 58, h = 7, x = 11, y = 9;
    for (;;) {
        char f0[16], f1[16], f2[16];
        if (date) {
            int m = v0;
            if (m < 1 || m > 12)
                m = 1;
            char century[3], year[3];
            snprintf(f0, sizeof(f0), "%s", AwardMonths[m - 1]);
            award_two_digits(f1, v1);
            award_two_digits(century, AwardCfg.century);
            award_two_digits(year, v2);
            snprintf(f2, sizeof(f2), "%s%s", century, year);
        } else {
            award_two_digits(f0, v0);
            award_two_digits(f1, v1);
            award_two_digits(f2, v2);
        }
        const char *labels[3];
        const char *values[3] = { f0, f1, f2 };
        int cols[3] = { x + 3, x + 22, x + 40 };
        if (date) {
            labels[0] = "Month:";
            labels[1] = "Day:";
            labels[2] = "Year:";
        } else {
            labels[0] = "Hour:";
            labels[1] = "Minute:";
            labels[2] = "Second:";
        }
        award_draw_shadow(x, y, w, h);
        int r;
        for (r = 1; r < h - 1; r++)
            award_vga_fill(y + r, x + 1, w - 2, ATTR_NORMAL);
        award_vga_box(x, y, w, h, ATTR_BOX);
        award_popup_title(x, w, y, date ? "System Date" : "System Time",
                          ATTR_TITLE);
        int i;
        for (i = 0; i < 3; i++) {
            int col = cols[i];
            award_vga_write(y + 2, col, labels[i], ATTR_NORMAL);
            col += strlen(labels[i]) + 1;
            if (i == field) {
                award_vga_fill(y + 2, col, 8, ATTR_SELECT);
                award_vga_write(y + 2, col, values[i], ATTR_SELECT);
            } else {
                award_vga_fill(y + 2, col, 8, ATTR_NORMAL);
                award_vga_write(y + 2, col, values[i], ATTR_NORMAL);
            }
        }
        award_popup_title(x, w, y + 4,
                          "Up/Down: Change   Left/Right: Field   Enter: Accept",
                          ATTR_NORMAL);
        int k = get_keystroke_full(100);
        if (k < 0)
            continue;
        int scan = k >> 8;
        int ascii = k & 0xff;
        if (scan == 0x01)
            return 0;
        if (scan == 0x1c) {
            if (date) {
                AwardCfg.month = v0;
                AwardCfg.day = v1;
                AwardCfg.year = v2;
            } else {
                AwardCfg.hour = v0;
                AwardCfg.minute = v1;
                AwardCfg.second = v2;
            }
            return changed;
        }
        if (scan == 0x4b || scan == 0x4d || scan == 0x0f) {
            if (scan == 0x4b) {
                if (field > 0)
                    field--;
                else
                    field = 2;
            } else {
                if (field < 2)
                    field++;
                else
                    field = 0;
            }
            continue;
        }
        int up = (scan == 0x48 || ascii == '+' || scan == 0x4e);
        int down = (scan == 0x50 || ascii == '-' || scan == 0x4a);
        if (!up && !down)
            continue;
        if (date) {
            if (field == 0) {
                if (up)
                    v0 = (v0 % 12) + 1;
                else
                    v0 = ((v0 + 10) % 12) + 1;
            } else if (field == 1) {
                int max = award_days_in_month(v0, v2);
                if (up) {
                    v1++;
                    if (v1 > max)
                        v1 = 1;
                } else {
                    if (v1 > 1)
                        v1--;
                    else
                        v1 = max;
                }
            } else {
                if (up)
                    v2 = (v2 + 1) % 100;
                else
                    v2 = (v2 + 99) % 100;
                int max = award_days_in_month(v0, v2);
                if (v1 > max)
                    v1 = max;
            }
        } else {
            if (field == 0) {
                if (up)
                    v0 = (v0 + 1) % 24;
                else
                    v0 = (v0 + 23) % 24;
            } else if (field == 1) {
                if (up)
                    v1 = (v1 + 1) % 60;
                else
                    v1 = (v1 + 59) % 60;
            } else {
                if (up)
                    v2 = (v2 + 1) % 60;
                else
                    v2 = (v2 + 59) % 60;
            }
        }
        changed = 1;
    }
}

static int
award_scsi_type_match(int type, u8 dtype)
{
    return (type == AWARD_SCSI_VIRTIO && dtype == DTYPE_VIRTIO_SCSI)
        || (type == AWARD_SCSI_LSI && dtype == DTYPE_LSI_SCSI)
        || (type == AWARD_SCSI_ESP && dtype == DTYPE_ESP_SCSI);
}

static int
award_scsi_drive_id(struct drive_s *d, int *target, int *lun)
{
    u8 type = GET_GLOBALFLAT(d->type);
    if (type == DTYPE_VIRTIO_SCSI)
        return virtio_scsi_drive_id(d, target, lun);
    if (type == DTYPE_LSI_SCSI)
        return lsi_scsi_drive_id(d, target, lun);
    if (type == DTYPE_ESP_SCSI)
        return esp_scsi_drive_id(d, target, lun);
    return -1;
}

static const char *
award_scsi_device(int type, int target, int *lun, struct drive_s **out)
{
    int i;
    for (i = 0; ; i++) {
        const char *desc = NULL;
        struct drive_s *d = boot_drive_at(i, &desc);
        if (!d)
            break;
        u8 dtype = GET_GLOBALFLAT(d->type);
        if (!award_scsi_type_match(type, dtype))
            continue;
        int tgt = -1, l = 0;
        if (award_scsi_drive_id(d, &tgt, &l) || tgt != target)
            continue;
        if (lun)
            *lun = l;
        if (out)
            *out = d;
        return desc;
    }
    return NULL;
}

static void
award_scsi_model(const char *desc, char *buf, int size)
{
    const char *p = desc;
    while (*p) {
        if (p[0] == ' ' && p[1] == 'D' && p[2] == 'r' && p[3] == 'i'
            && p[4] == 'v' && p[5] == 'e' && p[6] == ' ') {
            p += 7;
            break;
        }
        p++;
    }
    strtcpy(buf, *p ? p : desc, size);
}

static void
award_show_scsi_ids(int type)
{
    const char *title;
    int maxid;
    if (type == AWARD_SCSI_VIRTIO) {
        title = "VirtIO SCSI BIOS";
        maxid = 16;
    } else if (type == AWARD_SCSI_LSI) {
        title = "LSI 53C895A SCSI BIOS";
        maxid = 16;
    } else {
        title = "AMD 53C974A SCSI BIOS";
        maxid = 8;
    }
    award_draw_frame(title, 1);
    int i;
    for (i = 0; i < maxid; i++) {
        char buf[80];
        char idbuf[4];
        if (i < 10) {
            idbuf[0] = ' ';
            idbuf[1] = '0' + i;
            idbuf[2] = '\0';
        } else {
            idbuf[0] = '1';
            idbuf[1] = '0' + i - 10;
            idbuf[2] = '\0';
        }
        u8 attr = ATTR_NORMAL;
        if (i == 7) {
            snprintf(buf, sizeof(buf), "ID %s : Host Adapter", idbuf);
            attr = ATTR_VALUE;
        } else {
            int lun = 0;
            struct drive_s *d = NULL;
            const char *desc = award_scsi_device(type, i, &lun, &d);
            if (!desc) {
                snprintf(buf, sizeof(buf), "ID %s : No device", idbuf);
                attr = ATTR_HELP;
            } else if (GET_GLOBALFLAT(d->blksize) == CDROM_SECTOR_SIZE) {
                snprintf(buf, sizeof(buf), "ID %s : CD-ROM", idbuf);
            } else {
                char model[50];
                award_scsi_model(desc, model, sizeof(model));
                snprintf(buf, sizeof(buf), "ID %s : %s", idbuf, model);
            }
        }
        award_vga_write(4 + i, 3, buf, attr);
    }
    award_vga_write(22, 2, "Press any key to return", ATTR_NORMAL);
    while (get_keystroke_full(100) < 0)
        ;
}

static void
award_show_ide(void)
{
    static const char *const names[] = {
        "Primary Master", "Primary Slave",
        "Secondary Master", "Secondary Slave"
    };
    int i;
    award_draw_frame("IDE HDD Auto Detection", 1);
    for (i = 0; i < 4; i++) {
        char status[60];
        award_ide_status(i, status, sizeof(status));
        award_vga_write(5 + i * 2, 3, names[i], ATTR_NORMAL);
        award_vga_put(5 + i * 2, 20, ':', ATTR_NORMAL);
        award_vga_write(5 + i * 2, 22, status, ATTR_VALUE);
    }
    award_vga_write(22, 2, "Press any key to return", ATTR_NORMAL);
    while (get_keystroke_full(100) < 0)
        ;
}

static int
award_handle_action(u8 action)
{
    switch (action) {
    case ACT_SAVE_EXIT:
        return 1;
    case ACT_EXIT:
        if (award_confirm("Quit Without Saving (Y/N)?"))
            return -1;
        return 0;
    case ACT_LOAD_BIOS:
        if (award_confirm("Load BIOS Defaults (Y/N)?"))
            award_load_failsafe(&AwardCfg);
        return 0;
    case ACT_LOAD_SETUP:
        if (award_confirm("Load Setup Defaults (Y/N)?"))
            award_load_setup(&AwardCfg);
        return 0;
    case ACT_IDE_DETECT:
        award_show_ide();
        return 0;
    }
    return 0;
}

static int
award_ui(void)
{
    int mode = 0;
    int mcol = 0, mrow = 0;
    int item = 0, redraw = 1;
    const struct award_page *page = NULL;
    for (;;) {
        if (redraw) {
            if (mode == 0)
                award_draw_main(mcol, mrow);
            else
                award_draw_page(page, item);
            redraw = 0;
        }
        int k = get_keystroke_full(100);
        if (k < 0)
            continue;
        int scan = k >> 8;
        int ascii = k & 0xff;
        if (scan == 0x44)
            return 1;
        if (mode == 0) {
            const struct award_main_item *mi;
            int len;
            if (mcol == 0) {
                mi = MenuLeft;
                len = ARRAY_SIZE(MenuLeft);
            } else {
                mi = MenuRight;
                len = ARRAY_SIZE(MenuRight);
            }
            if (scan == 0x01) {
                if (award_confirm("Quit Without Saving (Y/N)?"))
                    return 0;
                redraw = 1;
                continue;
            }
            if (scan == 0x48) {
                if (mrow > 0)
                    mrow--;
                else
                    mrow = len - 1;
                redraw = 1;
                continue;
            }
            if (scan == 0x50) {
                if (mrow + 1 < len)
                    mrow++;
                else
                    mrow = 0;
                redraw = 1;
                continue;
            }
            if (scan == 0x4b || scan == 0x4d) {
                mcol = (scan == 0x4d) ? 1 : 0;
                if (mcol == 0 && mrow >= ARRAY_SIZE(MenuLeft))
                    mrow = ARRAY_SIZE(MenuLeft) - 1;
                if (mcol == 1 && mrow >= ARRAY_SIZE(MenuRight))
                    mrow = ARRAY_SIZE(MenuRight) - 1;
                redraw = 1;
                continue;
            }
            if (scan == 0x1c) {
                const struct award_main_item *sel = &mi[mrow];
                if (sel->page) {
                    page = sel->page;
                    item = 0;
                    mode = 1;
                } else {
                    int rc = award_handle_action(sel->action);
                    if (rc == 1)
                        return 1;
                    if (rc < 0)
                        return 0;
                }
                redraw = 1;
                continue;
            }
            continue;
        }
        if (scan == 0x01) {
            mode = 0;
            redraw = 1;
            continue;
        }
        if (scan == 0x48) {
            if (item > 0)
                item--;
            else
                item = page->nitems - 1;
            redraw = 1;
            continue;
        }
        if (scan == 0x50) {
            if (item + 1 < page->nitems)
                item++;
            else
                item = 0;
            redraw = 1;
            continue;
        }
        if (scan == 0x47) {
            item = 0;
            redraw = 1;
            continue;
        }
        if (scan == 0x4f) {
            item = page->nitems - 1;
            redraw = 1;
            continue;
        }
        if (scan == 0x49) {
            if (item >= 8)
                item -= 8;
            else
                item = 0;
            redraw = 1;
            continue;
        }
        if (scan == 0x51) {
            if (item + 8 < page->nitems)
                item += 8;
            else
                item = page->nitems - 1;
            redraw = 1;
            continue;
        }
        const struct award_item *it = &page->items[item];
        if (scan == 0x1c) {
            if (it->type == AI_ENUM) {
                if (it->id >= SCSI_ID_BASE && it->id < SCSI_ID_BASE + 3) {
                    award_show_scsi_ids(it->id - SCSI_ID_BASE);
                    redraw = 1;
                    continue;
                }
                int sel = award_popup(it->name, it->choices, it->nchoices,
                                      *it->value, 4 + item);
                if (sel >= 0)
                    *it->value = sel;
                redraw = 1;
                continue;
            }
            if (it->type == AI_DATE) {
                if (!AwardDateTouched)
                    award_rtc_read_date(&AwardCfg);
                if (award_edit_clock(1))
                    AwardDateTouched = 1;
                redraw = 1;
                continue;
            }
            if (it->type == AI_TIME) {
                if (!AwardTimeTouched)
                    award_rtc_read_time(&AwardCfg);
                if (award_edit_clock(0))
                    AwardTimeTouched = 1;
                redraw = 1;
                continue;
            }
        }
        if (it->type == AI_ENUM) {
            int up = (scan == 0x48 || ascii == '+' || scan == 0x4e);
            int down = (scan == 0x50 || ascii == '-' || scan == 0x4a);
            if (up || down) {
                u8 v = *it->value;
                if (v >= it->nchoices)
                    v = 0;
                if (up)
                    v = (v + 1) % it->nchoices;
                else
                    v = (v + it->nchoices - 1) % it->nchoices;
                *it->value = v;
                redraw = 1;
            }
        }
    }
}

static void
award_cpu_name(char *buf, int size)
{
    u32 eax, ebx, ecx, edx;
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        u32 *p = (u32 *)buf;
        cpuid(0x80000002, &p[0], &p[1], &p[2], &p[3]);
        cpuid(0x80000003, &p[4], &p[5], &p[6], &p[7]);
        cpuid(0x80000004, &p[8], &p[9], &p[10], &p[11]);
        buf[48] = '\0';
        char *s = nullTrailingSpace(buf);
        if (*s) {
            memmove(buf, s, strlen(s) + 1);
            return;
        }
    }
    cpuid(0, &eax, &ebx, &ecx, &edx);
    memcpy(buf, &ebx, 4);
    memcpy(buf + 4, &edx, 4);
    memcpy(buf + 8, &ecx, 4);
    buf[12] = '\0';
    char *s = nullTrailingSpace(buf);
    if (s != buf)
        memmove(buf, s, strlen(s) + 1);
    if (!*buf)
        snprintf(buf, size, "Unknown CPU");
}

static u32
award_cpu_mhz(void)
{
    if (AwardCpuMhzValid)
        return AwardCpuMhz;
    AwardCpuMhzValid = 1;
    u32 eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x16) {
        cpuid(0x16, &eax, &ebx, &ecx, &edx);
        AwardCpuMhz = eax & 0xffff;
        if (AwardCpuMhz)
            return AwardCpuMhz;
    }
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1 << 4)))
        return 0;
    u64 start = rdtscll();
    u32 end = timer_calc(50);
    while (!timer_check(end))
        yield();
    u32 delta = (u32)(rdtscll() - start);
    AwardCpuMhz = delta / 50000;
    return AwardCpuMhz;
}

static int
award_has_cpu_word(const char *s)
{
    while (*s) {
        if (s[0] == 'C' && s[1] == 'P' && s[2] == 'U')
            return 1;
        s++;
    }
    return 0;
}

static void
award_ide_post_status(int index, char *buf, int size)
{
    const char *desc = NULL;
    struct drive_s *d = award_ide_find(index >> 1, index & 1, &desc);
    if (!d) {
        snprintf(buf, size, "None");
        return;
    }
    u8 type = GET_GLOBALFLAT(d->type);
    if (type == DTYPE_ATA_ATAPI) {
        snprintf(buf, size, "CD-ROM");
        return;
    }
    if (desc && *desc) {
        const char *p = desc;
        if (p[0] == 'a' && p[1] == 't' && p[2] == 'a') {
            const char *q = strchr(p, ':');
            if (q)
                p = q + 1;
            while (*p == ' ')
                p++;
        }
        int i = 0;
        while (*p && i < size - 1) {
            if (p[0] == ' ' && p[1] == 'A' && p[2] == 'T'
                && p[3] == 'A' && p[4] == '-')
                break;
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        if (*buf)
            return;
    }
    snprintf(buf, size, "Hard Disk");
}

static int
award_scsi_count(int type)
{
    int i, count = 0;
    for (i = 0; ; i++) {
        const char *desc = NULL;
        struct drive_s *d = boot_drive_at(i, &desc);
        if (!d)
            break;
        u8 t = GET_GLOBALFLAT(d->type);
        if ((type == AWARD_SCSI_VIRTIO && t == DTYPE_VIRTIO_SCSI)
            || (type == AWARD_SCSI_LSI && t == DTYPE_LSI_SCSI)
            || (type == AWARD_SCSI_ESP && t == DTYPE_ESP_SCSI))
            count++;
    }
    return count;
}

static void
award_scsi_status(int type, char *buf, int size)
{
    int count = award_scsi_count(type);
    if (!count)
        snprintf(buf, size, "Not detected");
    else
        snprintf(buf, size, "Detected: %d device(s)", count);
}

static const char *
award_pci_short_name(u16 class)
{
    switch (class) {
    case 0x0600: return "Host Bridge";
    case 0x0601: return "ISA Bridge";
    case 0x0604: return "PCI Bridge";
    case 0x0c03: return "USB Controller";
    case 0x0101: return "IDE";
    case 0x0106: return "SATA";
    case 0x0300: return "VGA";
    case 0x0200: return "Network";
    case 0x0c05: return "SMBus";
    case 0x0401:
    case 0x0403: return "Audio";
    default:
        switch (class >> 8) {
        case 0x01: return "IDE";
        case 0x02: return "Network";
        case 0x03: return "VGA";
        case 0x04: return "Audio";
        case 0x05: return "Memory";
        case 0x06: return "Bridge Device";
        case 0x07: return "Communication";
        case 0x08: return "System";
        case 0x09: return "Input";
        case 0x0c: return "Serial Bus";
        default:   return "Device";
        }
    }
}

static const char *
award_pci_class_name(u16 class)
{
    switch (class) {
    case 0x0600: return "Host Bridge";
    case 0x0601: return "ISA Bridge";
    case 0x0604: return "PCI Bridge";
    case 0x0c03: return "USB Controller";
    case 0x0101: return "IDE Controller";
    case 0x0106: return "SATA Controller";
    case 0x0300: return "VGA Compatible";
    case 0x0200: return "Ethernet Controller";
    case 0x0280: return "Network Controller";
    case 0x0c05: return "SMBus Controller";
    case 0x0401:
    case 0x0403: return "Multimedia Device";
    case 0x0500: return "RAM Controller";
    case 0x0701: return "Parallel Controller";
    case 0x0700: return "Serial Controller";
    case 0x0800: return "Interrupt Controller";
    case 0x0801: return "DMA Controller";
    case 0x0802: return "Timer";
    case 0x0803: return "RTC";
    case 0x0900: return "Keyboard Controller";
    case 0x0c00: return "FireWire Controller";
    default:
        switch (class >> 8) {
        case 0x01: return "IDE Controller";
        case 0x02: return "Network Controller";
        case 0x03: return "Display Controller";
        case 0x04: return "Multimedia Device";
        case 0x05: return "Memory Controller";
        case 0x06: return "Bridge Device";
        case 0x07: return "Communication Device";
        case 0x08: return "System Peripheral";
        case 0x09: return "Input Device";
        case 0x0a: return "Docking Station";
        case 0x0b: return "Processor";
        case 0x0c: return "Serial Bus Controller";
        case 0x0d: return "Wireless Controller";
        default:   return "Unknown Device";
        }
    }
}

static void
award_pci_summary_name(struct pci_device *pci, char *buf, int size)
{
    const char *name = award_pci_short_name(pci->class);
    int bus = pci_bdf_to_bus(pci->bdf);
    if (bus == 0)
        snprintf(buf, size, "PCI Onboard %s", name);
    else
        snprintf(buf, size, "PCI Slot %d %s", bus, name);
    if (pci->class == 0x0300 || pci->class == 0x0200) {
        u8 irq = pci_config_readb(pci->bdf, PCI_INTERRUPT_LINE);
        if (irq && irq != 0xff) {
            int len = strlen(buf);
            snprintf(buf + len, size - len, ", IRQ%d", irq);
        }
    }
}

static void
award_serial_ports(char *buf, int size)
{
    int i, first = 1;
    buf[0] = '\0';
    for (i = 0; i < 4; i++) {
        u16 port = GET_BDA(port_com[i]);
        if (!port)
            continue;
        int len = strlen(buf);
        snprintf(buf + len, size - len, "%s%03X", first ? "" : ",", port);
        first = 0;
    }
    if (first)
        snprintf(buf, size, "None");
}

static void
award_parallel_ports(char *buf, int size)
{
    int i, first = 1;
    buf[0] = '\0';
    for (i = 0; i < 3; i++) {
        u16 port = GET_BDA(port_lpt[i]);
        if (!port)
            continue;
        int len = strlen(buf);
        snprintf(buf + len, size - len, "%s%03X", first ? "" : ",", port);
        first = 0;
    }
    if (first)
        snprintf(buf, size, "None");
}

static void
award_floppy_name(int type, char *buf, int size)
{
    static const char *const names[] = {
        "None",
        "360 KB 5\xAC\"",
        "1.2 MB 5\xAC\"",
        "720 KB 3\xAB\"",
        "1.44 MB 3\xAB\"",
        "2.88 MB 3\xAB\""
    };
    if (type < 0 || type >= ARRAY_SIZE(names))
        type = 0;
    strtcpy(buf, names[type], size);
}

static void
award_line_clear(char *line)
{
    int i;
    for (i = 0; i < 79; i++)
        line[i] = ' ';
    line[79] = '\0';
}

static void
award_line_write(char *line, int col, const char *s)
{
    while (*s && col < 79)
        line[col++] = *s++;
}

static void
award_line_put(char *line, int col, char c)
{
    if (col >= 0 && col < 79)
        line[col] = c;
}

static void
award_line_border(char *line, char left, char right, char fill)
{
    int i;
    line[0] = left;
    for (i = 1; i < 78; i++)
        line[i] = fill;
    line[78] = right;
    line[79] = '\0';
}

static void
award_post_line(const char *line)
{
    printf("%s\n", line);
}

static void
award_set_cursor(int row, int col)
{
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.ax = 0x0200;
    br.bx = 0;
    br.dx = (row << 8) | col;
    br.flags = F_IF;
    call16_int(0x10, &br);
}

static void
award_draw_award_post(void)
{
    static const char *const labels[] = {
        "Primary Master : ",
        "Primary Slave  : ",
        "Secondary Master : ",
        "Secondary Slave : "
    };
    char buf[80];
    char cpu[64];
    int i, r;
    for (r = 0; r < 25; r++)
        award_vga_fill(r, 0, 80, ATTR_POST);
    award_vga_write(1, 0,
                    "MoeRain BIOS v4.51PG, An Energy Star Ally",
                    ATTR_POST_TITLE);
    award_vga_write(2, 0, "Copyright (C) 2020-2026, MoeRain Software Developments",
                    ATTR_POST);
    award_vga_write(4, 0, "ROM PCI/ISA BIOS (2A69KQ1C)", ATTR_POST);
    award_cpu_name(cpu, sizeof(cpu));
    u32 mhz = award_cpu_mhz();
    if (mhz && !strchr(cpu, '@'))
        snprintf(buf, sizeof(buf), "%s%s at %uMHz", cpu,
                 award_has_cpu_word(cpu) ? "" : " CPU", mhz);
    else
        snprintf(buf, sizeof(buf), "%s", cpu);
    award_vga_write(6, 0, buf, ATTR_POST);
    u32 memk = RamSize / 1024;
    if (!memk)
        memk = 640;
    snprintf(buf, sizeof(buf), "Memory Test : %uK OK", memk);
    award_vga_write(8, 0, buf, ATTR_POST);
    award_vga_write(10, 0,
                    "MoeRain Plug and Play BIOS Extension v1.0A",
                    ATTR_POST);
    award_vga_write(11, 0, "Copyright (C) 2026, MoeRain Software Developments",
                    ATTR_POST);
    award_vga_write(13, 0, "Detecting IDE Primary Master......",
                    ATTR_POST);
    for (i = 0; i < 4; i++) {
        char status[48];
        award_ide_post_status(i, status, sizeof(status));
        award_vga_write(14 + i, 0, labels[i], ATTR_POST);
        award_vga_write(14 + i, strlen(labels[i]), status, ATTR_POST);
    }
    award_vga_write(22, 0,
                    "Press DEL or F2 to enter SETUP",
                    ATTR_POST_PROMPT);
    award_vga_write(24, 0, "10/21/2026-QEMU-i440FX-2A69KQ1C-00", ATTR_POST);
    award_logo_draw();
}

static void
award_draw_post1(void)
{
    char line[80];
    char buf[80];
    char cpu[64];
    char floppy[24];
    char ports[40];
    int i;
    award_line_border(line, 0xc9, 0xbb, 0xcd);
    award_post_line(line);
    award_line_clear(line);
    award_line_write(line, 2,
                     "MoeRain BIOS System Configuration (C) 2020-2026, MoeRain Software Developments.,");
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_post_line(line);
    award_line_border(line, 0xcc, 0xb9, 0xcd);
    award_post_line(line);
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_line_write(line, 2, "Main Processor   : ");
    award_cpu_name(cpu, sizeof(cpu));
    award_line_write(line, 21, cpu);
    award_post_line(line);
    for (i = 0; i < 5; i++) {
        award_line_clear(line);
        award_line_put(line, 0, 0xba);
        award_line_put(line, 41, 0xba);
        award_line_put(line, 78, 0xba);
        switch (i) {
        case 0:
            award_line_write(line, 2, "Math Processor   : Built-In");
            award_line_write(line, 43, "Base Memory Size : 640KB");
            break;
        case 1: {
            award_floppy_name(AwardCfg.floppy_a, floppy, sizeof(floppy));
            snprintf(buf, sizeof(buf), "Floppy Drive A:  : %s", floppy);
            award_line_write(line, 2, buf);
            u32 memk = RamSize / 1024;
            u32 ext = memk > 1024 ? memk - 1024 : 0;
            snprintf(buf, sizeof(buf), "Ext. Memory Size : %uKB", ext);
            award_line_write(line, 43, buf);
            break;
        }
        case 2:
            award_floppy_name(AwardCfg.floppy_b, floppy, sizeof(floppy));
            snprintf(buf, sizeof(buf), "Floppy Drive B:  : %s", floppy);
            award_line_write(line, 2, buf);
            award_line_write(line, 43, "Display Type     : VGA/EGA");
            break;
        case 3:
            award_line_write(line, 2, "BIOS Date        : 10/21/2026");
            award_serial_ports(ports, sizeof(ports));
            snprintf(buf, sizeof(buf), "Serial Port(s)   : %s", ports);
            award_line_write(line, 43, buf);
            break;
        case 4: {
            u32 mhz = award_cpu_mhz();
            if (mhz)
                snprintf(buf, sizeof(buf), "Processor Clock  : %uMHz", mhz);
            else
                snprintf(buf, sizeof(buf), "Processor Clock  : N/A");
            award_line_write(line, 2, buf);
            award_parallel_ports(ports, sizeof(ports));
            snprintf(buf, sizeof(buf), "Parallel Port(s) : %s", ports);
            award_line_write(line, 43, buf);
            break;
        }
        }
        award_post_line(line);
    }
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 41, 0xba);
    award_line_put(line, 78, 0xba);
    award_post_line(line);
    award_line_border(line, 0xcc, 0xb9, 0xcd);
    award_post_line(line);
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_line_write(line, 2, "ATA(PI) Device(s)");
    award_line_write(line, 22, "Type");
    award_line_write(line, 34, "Size");
    award_line_write(line, 46, "LBA");
    award_line_write(line, 52, "32Bit");
    award_line_write(line, 60, "Block");
    award_line_write(line, 68, "PIO");
    award_post_line(line);
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_line_write(line, 46, "Mode");
    award_line_write(line, 52, "Mode");
    award_line_write(line, 60, "Mode");
    award_line_write(line, 68, "Mode");
    award_post_line(line);
    {
        static const char *const labels[] = {
            "Primary Master   : ",
            "Primary Slave    : ",
            "Secondary Master : ",
            "Secondary Slave  : "
        };
        int printed = 0;
        for (i = 0; i < 4; i++) {
            struct drive_s *d = award_ide_find(i >> 1, i & 1, NULL);
            if (!d)
                continue;
            award_line_clear(line);
            award_line_put(line, 0, 0xba);
            award_line_put(line, 78, 0xba);
            award_line_write(line, 2, labels[i]);
            int iscd = (GET_GLOBALFLAT(d->blksize) == CDROM_SECTOR_SIZE);
            award_line_write(line, 22, iscd ? "ATAPI CDROM" : "Hard Disk");
            if (!iscd) {
                u32 sectors = (u32)GET_GLOBALFLAT(d->sectors);
                snprintf(buf, sizeof(buf), "%uMB", sectors / 2048);
                award_line_write(line, 34, buf);
                award_line_write(line, 46, "LBA");
                award_line_write(line, 52, CONFIG_ATA_PIO32 ? "On" : "Off");
                award_line_write(line, 60, "32Sec");
            }
            award_line_write(line, 68, "4");
            award_post_line(line);
            printed++;
        }
        for (; printed < 4; printed++) {
            award_line_clear(line);
            award_line_put(line, 0, 0xba);
            award_line_put(line, 78, 0xba);
            award_post_line(line);
        }
    }
    award_line_border(line, 0xcc, 0xb9, 0xcd);
    award_post_line(line);
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_line_write(line, 2, "PCI Devices:");
    award_post_line(line);
    {
        int count = 0;
        struct pci_device *pci;
        foreachpci(pci) {
            if (count >= 6)
                break;
            if ((count & 1) == 0) {
                award_line_clear(line);
                award_line_put(line, 0, 0xba);
                award_line_put(line, 78, 0xba);
            }
            award_pci_summary_name(pci, buf, sizeof(buf));
            award_line_write(line, (count & 1) ? 42 : 2, buf);
            count++;
            if ((count & 1) == 0)
                award_post_line(line);
        }
        if (count & 1)
            award_post_line(line);
        int used = (count + 1) / 2;
        for (i = used; i < 3; i++) {
            award_line_clear(line);
            award_line_put(line, 0, 0xba);
            award_line_put(line, 78, 0xba);
            award_post_line(line);
        }
    }
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_post_line(line);
    award_line_clear(line);
    award_line_put(line, 0, 0xba);
    award_line_put(line, 78, 0xba);
    award_line_write(line, 2, "build date: 2026-10-21");
    award_post_line(line);
    award_line_border(line, 0xc8, 0xbc, 0xcd);
    award_post_line(line);
}

static void
award_draw_post2(void)
{
    char line[80];
    char buf[80];
    int row = 0;
    award_line_clear(line);
    award_line_write(line, 2, "PCI Device Listing ...");
    award_post_line(line);
    award_line_clear(line);
    award_line_write(line, 2,
                     "Bus No.    Device No. Func No.   Vendor ID  Device ID  Device Class");
    award_post_line(line);
    struct pci_device *pci;
    foreachpci(pci) {
        if (row >= 14)
            break;
        char cls[22];
        strtcpy(cls, award_pci_class_name(pci->class), sizeof(cls));
        int bus = pci_bdf_to_bus(pci->bdf);
        int dev = pci_bdf_to_dev(pci->bdf);
        int fn = pci_bdf_to_fn(pci->bdf);
        snprintf(buf, sizeof(buf),
                 "%3X         %2X         %X          %04X       %04X       %s",
                 bus, dev, fn, pci->vendor, pci->device, cls);
        award_line_clear(line);
        award_line_write(line, 2, buf);
        award_post_line(line);
        row++;
    }
    award_line_clear(line);
    award_post_line(line);
    u32 memk = RamSize / 1024;
    snprintf(buf, sizeof(buf), "SDRAM          : %uK", memk);
    award_line_clear(line);
    award_line_write(line, 2, buf);
    award_post_line(line);
    const char *bootdev = "Hard Disk";
    int bd = award_boot_device(0);
    if (bd == AWARD_BOOT_FLOPPY)
        bootdev = "Floppy";
    else if (bd == AWARD_BOOT_CDROM)
        bootdev = "CDROM";
    else if (bd == AWARD_BOOT_NETWORK)
        bootdev = "Network";
    snprintf(buf, sizeof(buf), "Searching for Boot Record from %s..", bootdev);
    award_line_clear(line);
    award_line_write(line, 2, buf);
    award_post_line(line);
    award_line_clear(line);
    award_line_write(line, 2, "Verifying DMI Pool Data ............");
    award_post_line(line);
    award_line_clear(line);
    award_post_line(line);
    award_line_clear(line);
    award_line_write(line, 2, "Successfully Loaded DMI Data");
    award_post_line(line);
}

static int
award_draw_post(void)
{
    int rc;
    award_draw_award_post();
    rc = award_post_wait(2000, 1);
    award_logo_clear();
    award_logo_font_restore();
    if (rc)
        return rc;
    award_set_cursor(24, 0);
    printf("\n");
    award_draw_post1();
    award_draw_post2();
    rc = award_post_wait(2000, 0);
    if (rc)
        return rc;
    return 0;
}

static int
award_post_wait(int msec, int allow_setup)
{
    int k;
    while ((k = get_keystroke_full(0)) >= 0) {
        int scan = k >> 8;
        if (allow_setup && (scan == 0x3c || scan == 0x53))
            return 1;
        if (scan == 0x01)
            return 2;
    }
    u32 end = irqtimer_calc(msec);
    for (;;) {
        k = get_keystroke_full(100);
        if (k >= 0) {
            int scan = k >> 8;
            if (allow_setup && (scan == 0x3c || scan == 0x53))
                return 1;
            if (scan == 0x01)
                return 2;
        }
        if (irqtimer_check(end))
            return 0;
    }
}

void
award_bios_setup(void)
{
    award_init();
    award_numlock();
    award_vga_mode();
    int rc = award_draw_post();
    if (rc == 0) {
        boot_menu_skip();
        return;
    }
    if (rc == 2) {
        interactive_bootmenu_force();
        return;
    }
    award_rtc_read_date(&AwardCfg);
    award_rtc_read_time(&AwardCfg);
    AwardDateTouched = 0;
    AwardTimeTouched = 0;
    int save = award_ui();
    if (save) {
        if (AwardDateTouched || AwardTimeTouched) {
            if (!AwardDateTouched)
                award_rtc_read_date(&AwardCfg);
            if (!AwardTimeTouched)
                award_rtc_read_time(&AwardCfg);
            award_rtc_write();
        }
        award_save();
    }
    award_vga_mode();
    if (save)
        qemu_reboot();
}

#endif

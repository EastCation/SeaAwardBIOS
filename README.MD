# SeaAwardBIOS

**English** | [简体中文](README.zh-CN.md)

SeaAwardBIOS is a [SeaBIOS](https://www.seabios.org/)-based firmware that
recreates the look and feel of an Award BIOS-era x86 legacy system. It keeps
the SeaBIOS boot foundation while adding an Award-style POST screen, a
CMOS Setup Utility, a boot device priority configuration, and persistent
settings backed by QEMU pflash NVRAM.

It is intended for legacy BIOS emulation, firmware development, retro
computing experiments, and QEMU-based testing.

## Screenshots

| Award-style POST | CMOS Setup Utility |
| :---: | :---: |
| ![Award-style POST screen](assets/docs/BIOSPOST.png) | ![Award-style CMOS Setup Utility](assets/docs/CMOSSETUP.png) |

## Features

- **Award-style POST** — logo, CPU and memory detection, IDE device
  summary, PCI device listing, DMI pool messages, and boot device search.
- **CMOS Setup Utility** — enter with `Del` or `F2` during POST. Navigate
  with the arrow keys, `Page Up`/`Page Down`, `+`/`-`, `Enter`, and `Esc`.
- **Persistent NVRAM** — settings are stored in a second 256 KiB QEMU
  pflash device, using two checksummed, alternating flash sectors.
- **Award-controlled boot order** — first, second, and third boot devices
  can be set to `Floppy`, `Hard Disk`, `CDROM`, or `Network`.
- **Integrated peripheral control** — enable or disable IDE channels and
  devices, the USB controller, the floppy controller, serial ports, and the
  parallel port.
- **SCSI BIOS configuration** — enable or disable the virtio-scsi,
  LSI 53C895A, and AMD 53C974A option ROMs, and inspect detected SCSI IDs.
- **Power management** — enable or disable ACPI support.
- **Boot menu integration** — press `Esc` during the Award POST to open the
  SeaBIOS boot device menu; duplicate boot menus are suppressed.
- **Based on SeaBIOS** — retains SeaBIOS hardware support, option ROM
  handling, SeaVGABIOS, and the standard legacy BIOS interfaces.

## Quick start

### Requirements

- A GNU build environment: `gcc`, `binutils`, `make`, `python`, and a
  POSIX-compatible shell (Linux, WSL, MSYS2, or similar).
- QEMU for x86 (`qemu-system-x86_64`).
- PowerShell 7+ if you want to use the provided Windows launcher script.

### Build

```bash
make
```

The resulting firmware image is `out/bios.bin` and is expected to be exactly
256 KiB (262144 bytes).

### Run with persistent settings

On Windows, the provided script creates a 256 KiB NVRAM image on first run
and launches QEMU with both pflash devices:

```powershell
pwsh -File .\scripts\run-award.ps1
```

Extra QEMU arguments can be appended directly:

```powershell
pwsh -File .\scripts\run-award.ps1 -Memory 256M -drive "file=disk.img,format=raw,if=ide"
```

To run manually, create an NVRAM image once and start QEMU with the BIOS
mapped read-only and the NVRAM mapped writable:

```bash
python3 -c "open('out/nvram.bin','wb').write(b'\xff' * 262144)"

qemu-system-x86_64 -machine pc -m 128M \
  -drive if=pflash,format=raw,readonly=on,file=out/bios.bin \
  -drive if=pflash,format=raw,file=out/nvram.bin
```

> **Important:** Award Setup requires the two-pflash layout. Booting with
> only `-bios out/bios.bin` still displays the Award POST and Setup screens,
> but settings cannot persist across power cycles.

## Using the CMOS Setup Utility

During the first Award POST screen, press:

| Key | Action |
| --- | --- |
| `Del` or `F2` | Enter the CMOS Setup Utility |
| `Esc` | Open the SeaBIOS boot device menu |

Inside Setup:

| Key | Action |
| --- | --- |
| `↑` / `↓` | Move between items |
| `←` / `→` | Switch between the left and right menu columns |
| `Enter` | Open a page, select a value, or show the SCSI ID list |
| `+` / `-` | Change an enumerated value |
| `Page Up` / `Page Down` | Jump one page in a menu |
| `Esc` | Return to the previous screen or quit without saving |
| `F10` | Save settings and reboot |

### Setup pages

| Menu | Options |
| --- | --- |
| Standard CMOS Features | Date, time, IDE primary/secondary master/slave, floppy drives A/B, detected memory sizes |
| Advanced BIOS Features | First/second/third boot device, Boot Other Device, Boot Up NumLock Status |
| Integrated Peripherals | On-chip primary/secondary PCI IDE, USB controller, serial ports 1/2, parallel port, FDD controller |
| SCSI BIOS Configuration | VirtIO SCSI BIOS, LSI 53C895A SCSI BIOS, AMD 53C974A SCSI BIOS; `Enter` shows detected SCSI IDs |
| Power Management Setup | ACPI Function |
| Load Fail-Safe Defaults | Restore conservative defaults (floppy first, NumLock off) |
| Load Optimized Defaults | Restore optimized defaults (hard disk first, NumLock on) |
| IDE HDD Auto Detection | Display auto-detected IDE devices |
| Save & Exit Setup | Write settings to NVRAM and reboot |
| Exit Without Saving | Discard changes and reboot |

The default boot order is `Hard Disk`, `CDROM`, `Floppy`; fail-safe defaults
use `Floppy`, `Hard Disk`, `CDROM`. `Boot Other Device` is enabled by default.

Hardware-related settings are enforced during initialization and boot:
disabling an IDE channel, the floppy controller, a SCSI option ROM, USB, or
ACPI prevents the corresponding device or service from being initialized.

## NVRAM and persistence

- The second pflash device is mapped at `0xfff80000`; SeaAwardBIOS reserves
  the top 512 KiB of the address space for the 256 KiB BIOS and 256 KiB
  NVRAM.
- Settings are stored as a magic-tagged, checksummed record in one of two
  4 KiB flash sectors. Saves alternate between sectors, and the newest valid
  record wins, so an interrupted write does not destroy the previous settings.
- If the NVRAM image is missing, invalid, or read-only, SeaAwardBIOS falls
  back to built-in defaults and reports that settings will not persist.
- Delete `out/nvram.bin` to reset all settings to defaults.

## Configuration

SeaAwardBIOS uses the standard SeaBIOS kconfig system:

```bash
make menuconfig
make
```

The Award-specific option is `CONFIG_AWARD_SETUP`:

> Award-style CMOS setup with QEMU pflash NVRAM

It depends on `CONFIG_QEMU` and `CONFIG_BOOT`, is enabled by default, and
requires `CONFIG_BOOTMENU` for the integrated boot menu. All other SeaBIOS
options continue to work as documented by upstream SeaBIOS.

## Project layout

| Path | Description |
| --- | --- |
| `src/award.c`, `src/award.h` | Award-style POST, Setup UI, NVRAM records, device policy |
| `src/award_logo.h` | Font data for the POST logo |
| `src/boot.c` | Award boot order integration and boot menu changes |
| `src/post.c` | POST integration and USB policy |
| `src/hw/ata.c`, `src/hw/floppy.c` | IDE and floppy controller policy |
| `src/serial.c`, `src/fw/paravirt.c` | Serial/parallel port and ACPI policy |
| `scripts/run-award.ps1` | Windows launcher for the two-pflash QEMU layout |
| `assets/docs/` | Screenshots used in the documentation |
| `docs/` | SeaBIOS developer documentation |

## Documentation

- [Build overview](docs/Build_overview.md)
- [Execution and code flow](docs/Execution_and_code_flow.md)
- [Memory model](docs/Memory_Model.md)
- [Developer documentation](docs/Developer_Documentation.md)
- [SeaVGABIOS](docs/SeaVGABIOS.md)
- [Contributing](docs/Contributing.md)
- [Upstream SeaBIOS documentation](https://www.seabios.org/Developer_Documentation.html)

## Contributing

SeaAwardBIOS follows the SeaBIOS contribution process. Patches should be
tested locally and include a `Signed-off-by` line using a real name and a
current email address, indicating agreement with the developer certificate
of origin. See [docs/Contributing.md](docs/Contributing.md) for details.

## License

SeaAwardBIOS is based on SeaBIOS and is distributed under the GNU Lesser
General Public License, version 3 or later. See [COPYING.LESSER](COPYING.LESSER)
and [COPYING](COPYING) for the full license texts.

SeaAwardBIOS is an independent project and is not affiliated with or
endorsed by Award Software. "Award" is used only to describe the
compatibility-oriented visual style.

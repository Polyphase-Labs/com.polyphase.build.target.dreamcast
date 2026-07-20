# DiscImage — third-party notices

This directory implements the addon's built-in, in-process Dreamcast self-boot
CDI writer. It is adapted from **mkdcdisc** (https://gitlab.com/simulant/mkdcdisc)
and deliberately excludes mkdcdisc's GPL dependency (`libisofs`), which is
replaced by the first-party `DcIso9660` writer.

## Licenses of the files in this directory

| File(s) | Origin | License |
| ------- | ------ | ------- |
| `disc_image.c/.h`, `cdi.c`, `private.h` | mkdcdisc | **MIT** (© 2022 simulant) |
| `scramble.cpp/.h` | mkdcdisc / Marcus Comstedt | **Public Domain** |
| `elf_parser.cpp/.hpp`, `elf.h` | mkdcdisc | **MIT** (© 2018 finixbit; © 2022–2023 Colton Pawielski) |
| `IpBinTemplate.inc`, `DefaultMr.inc` | mkdcdisc (`IP.BIN`, `default.mr`) | mkdcdisc (MIT repo); IP.BIN bootstrap is Sega boot data redistributed by the DC homebrew community |
| `edc/edc_ecc.c`, `edc/libedc.c`, `edc/ecc.h`, `edc/edc.h`, `edc/*_table`, `edc/crctable.out`, `edc/encoder_tables` | cdrtools (Heiko Eissfeldt, Jörg Schilling) | **CDDL 1.0** (file-level copyleft; kept with headers, source available) |
| `edc/patch.c/.h` | SiZiOUS | permissive compile shim |
| `DcIso9660.{h,cpp}`, `DcIpBin.{h,cpp}`, `DcDiscBuilder.{h,cpp}` | Polyphase (first-party) | project license |

## Notes

- **No GPL.** mkdcdisc's `libisofs` (GPL v2+) is NOT vendored. ISO9660+Joliet
  generation is done by the first-party `DcIso9660`.
- **CDDL** applies only to the `edc/` Reed-Solomon EDC/ECC files. CDDL is
  file-level copyleft (not GPL) and is compatible with distributing this addon:
  keep those files' license headers intact and make their source available.
- `cdi.c` was modified for MSVC: the GCC `__attribute__((packed))` structs are
  wrapped in a portable `#pragma pack` region. `disc_image.c` had a C99 VLA
  replaced with a heap allocation. Behavior is otherwise unchanged.

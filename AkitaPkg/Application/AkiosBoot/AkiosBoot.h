/** @file
  AkiosBoot: the parts shared by its files.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef AKIOS_BOOT_H_
#define AKIOS_BOOT_H_

#include <Uefi.h>

//
// The boot payload (tools: ak; the format xnuloader's mkpayload.py made, kept so U-Boot's hakios command, which
// copies it to PAYLOAD_BASE, stays as it is): a header { u32 "XNUP", u32 bytes after it }, then entries
// { char name[16], u32 size, u32 reserved, data padded to 16 bytes }, up to one with an empty name.
//
#define AKIOS_PAYLOAD_BASE   0xF0000000ULL
#define AKIOS_PAYLOAD_MAGIC  0x50554E58u      // "XNUP"
#define AKIOS_PAYLOAD_MAX    0x04000000ULL    // the area U-Boot reserves for it

typedef struct {
  CONST VOID  *Data;
  UINTN       Size;
} AKIOS_ITEM;

/** The payload's entry NAME, or FALSE when there is no payload or no such entry. */
BOOLEAN
AkiosPayloadItem (
  IN  CONST CHAR8  *Name,
  OUT AKIOS_ITEM   *Item
  );

/** How many entries the payload has (0: none found); their names into the log. */
UINTN
AkiosPayloadSurvey (
  VOID
  );

/** Publish DATA (a FAT image, 512-byte sectors) as a read-only block device with a vendor device path. */
EFI_STATUS
AkiosPublishDisk (
  IN CONST VOID  *Data,
  IN UINTN       Size
  );

/** The log: kept in memory, written to the logdump partition from its start. */
VOID
AkiosLog (
  IN CONST CHAR8  *Format,
  ...
  );

VOID
EFIAPI
AkiosLogLine (
  IN CONST CHAR8  *Line,
  IN UINTN        Length
  );

/** Also print LINE on the console (through its original OutputString, set in gAkiosConsoleOutput). */
extern EFI_TEXT_STRING  gAkiosConsoleOutput;

VOID
AkiosLogEcho (
  IN CONST CHAR8  *Line
  );

/** Write the log so far to logdump (whole blocks, from block 0); quietly nothing when there is no such partition. */
VOID
AkiosLogFlush (
  VOID
  );

#endif

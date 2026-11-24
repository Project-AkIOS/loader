/** @file
  UniXNU boot manager core.

  Both the UEFI application and the DXE driver builds are thin wrappers around
  UniXNURun(); everything else lives in UniXNULib.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UNIXNU_LIB_H_
#define UNIXNU_LIB_H_

#include <Uefi.h>

typedef enum {
  ///
  /// Started as an ordinary UEFI application (from a disk, the shell, a
  /// firmware boot option...).
  ///
  UniXNUHostApplication,
  ///
  /// Started by the platform BDS through the boot option the DXE driver
  /// registered for itself.
  ///
  UniXNUHostFirmware
} UNIXNU_HOST;

/**
  Run the boot manager: locate and parse the configuration, inject drivers,
  show the menu and start the selected entry.

  @param[in] ImageHandle  Image handle of the running UniXNU image.
  @param[in] Host         How this image was started.

  @retval EFI_SUCCESS   The user chose to leave UniXNU.
  @retval EFI_ABORTED   Nothing could be booted.
  @retval other         Error from the boot path.
**/
EFI_STATUS
EFIAPI
UniXNURun (
  IN EFI_HANDLE      ImageHandle,
  IN UNIXNU_HOST  Host
  );

/**
  AkIOS: where a host keeps UniXNU's log lines (each with its "UNIXNU: " prefix and CR LF), besides the serial
  port and the debug channel -- the phone has no serial console, so AkIOS's boot application writes them to a
  log partition. NULL (the default): no copy.
**/
typedef
VOID
(EFIAPI *UNIXNU_LOG_SINK)(
  IN CONST CHAR8  *Line,
  IN UINTN        Length
  );

extern UNIXNU_LOG_SINK  gUniXNULogSink;

#endif

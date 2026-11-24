/** @file
  AkiosBoot.efi: what U-Boot starts on akita (its hakios command copies the boot payload to AKIOS_PAYLOAD_BASE and
  runs the EFI image next to it).

    1. the log: UniXNU's lines and everything printed on the console are kept and written to logdump as they come
    2. the payload's "esp" entry, a FAT image, becomes a read-only disk; its "fat" entry, EDK2's FAT driver, is
       started from memory (U-Boot's own FAT is not built into the phone's U-Boot)
    3. UniXNU's boot manager runs: it connects the disk, finds \EFI\UniXNU\akios.toml there, loads the drivers it
       names (DarwinBoot.efi) and starts the default entry

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AkiosBoot.h"

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UniXNULib.h>

STATIC
UINT32
CurrentEl (
  VOID
  )
{
  UINT64  El;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  return (UINT32)((El >> 2) & 3);
}

//
// The console's OutputString, wrapped: what DarwinBoot and the firmware print reaches the log too.
//
STATIC EFI_TEXT_STRING  mOutputString;
STATIC CHAR8            mConsoleLine[256];
STATIC UINTN            mConsoleLength;

STATIC
VOID
ConsoleLineEnd (
  VOID
  )
{
  if (mConsoleLength == 0) {
    return;
  }

  mConsoleLine[mConsoleLength++] = '\n';
  AkiosLogLine ("CON: ", 5);
  AkiosLogLine (mConsoleLine, mConsoleLength);
  mConsoleLength = 0;
  AkiosLogFlush ();
}

STATIC
EFI_STATUS
EFIAPI
LoggedOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  CHAR16  *At;

  for (At = String; (At != NULL) && (*At != 0); At++) {
    if (*At == L'\n') {
      ConsoleLineEnd ();
    } else if ((*At != L'\r') && (mConsoleLength < sizeof (mConsoleLine) - 1)) {
      mConsoleLine[mConsoleLength++] = (*At < 0x80) ? (CHAR8)*At : '?';
    }
  }

  return mOutputString (This, String);
}

STATIC
VOID
EFIAPI
UniXNULine (
  IN CONST CHAR8  *Line,
  IN UINTN        Length
  )
{
  AkiosLogLine (Line, Length);
  AkiosLogEcho (Line);
  AkiosLogFlush ();       // each line at once: the next one may be the last before XNU takes the machine
}

//
// The FAT driver from the payload, started from memory; UniXNU's ConnectAll binds it to the disk.
//
STATIC
EFI_STATUS
StartFatDriver (
  IN EFI_HANDLE  ImageHandle
  )
{
  AKIOS_ITEM  Fat;
  EFI_HANDLE  Driver;
  EFI_STATUS  Status;

  if (!AkiosPayloadItem ("fat", &Fat)) {
    AkiosLog ("no \"fat\" entry in the payload: the ESP image cannot be read");
    return EFI_NOT_FOUND;
  }

  Driver = NULL;
  Status = gBS->LoadImage (FALSE, ImageHandle, NULL, (VOID *)Fat.Data, Fat.Size, &Driver);
  if (!EFI_ERROR (Status)) {
    Status = gBS->StartImage (Driver, NULL, NULL);
  }

  AkiosLog ("FAT driver (%u bytes): %r", (UINT32)Fat.Size, Status);
  return Status;
}

//
// The failsafe (the payload's "failsafe" entry, decimal seconds; ak sets it for test boots, never for a flashed
// image): a firmware timer that writes the log and restarts the phone -- into Android when the boot was a RAM boot
// -- should nothing have taken the machine by then (a boot that failed back into the menu, a hang before XNU).
// ExitBootServices ends it: from there on the kernel's own safety nets (usb_lost_reboot, test_reboot_after) apply.
//
STATIC
VOID
EFIAPI
FailsafeFired (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  AkiosLog ("failsafe: %u s passed and nothing has taken the machine: restarting", (UINT32)(UINTN)Context);
  AkiosLogFlush ();
  gRT->ResetSystem (EfiResetCold, EFI_TIMEOUT, 0, NULL);
}

STATIC
VOID
ArmFailsafe (
  VOID
  )
{
  AKIOS_ITEM   Item;
  CONST CHAR8  *At;
  UINTN        Index;
  UINT64       Seconds;
  EFI_EVENT    Event;
  EFI_STATUS   Status;

  if (!AkiosPayloadItem ("failsafe", &Item)) {
    return;
  }

  Seconds = 0;
  At      = Item.Data;
  for (Index = 0; Index < Item.Size && At[Index] >= '0' && At[Index] <= '9' && Seconds < 100000; Index++) {
    Seconds = Seconds * 10 + (At[Index] - '0');
  }

  if (Seconds == 0) {
    return;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, FailsafeFired, (VOID *)(UINTN)Seconds, &Event);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (Event, TimerRelative, Seconds * 10000000ULL);
  }

  AkiosLog ("failsafe: restart in %u s unless XNU starts: %r", (UINT32)Seconds, Status);
}

EFI_STATUS
EFIAPI
AkiosBootEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  AKIOS_ITEM  Esp;
  EFI_STATUS  Status;

  if ((gST->ConOut != NULL) && (gST->ConOut->OutputString != NULL)) {
    mOutputString               = gST->ConOut->OutputString;
    gAkiosConsoleOutput         = mOutputString;
    gST->ConOut->OutputString   = LoggedOutputString;
  }

  gUniXNULogSink = UniXNULine;

  AkiosLog ("AkiosBoot: firmware %s rev 0x%x, CurrentEL %u", gST->FirmwareVendor != NULL ? gST->FirmwareVendor : L"?",
    gST->FirmwareRevision, CurrentEl ());
  if (AkiosPayloadSurvey () == 0) {
    AkiosLog ("no boot payload at 0x%lx", AKIOS_PAYLOAD_BASE);
  }

  if (!AkiosPayloadItem ("esp", &Esp)) {
    AkiosLog ("no \"esp\" entry in the payload");
  } else {
    Status = AkiosPublishDisk (Esp.Data, Esp.Size);
    AkiosLog ("ESP image (%u bytes) as a disk: %r", (UINT32)Esp.Size, Status);
  }

  StartFatDriver (ImageHandle);
  ArmFailsafe ();
  AkiosLogFlush ();

  Status = UniXNURun (ImageHandle, UniXNUHostApplication);
  AkiosLog ("UniXNU returned: %r", Status);
  AkiosLogFlush ();
  return Status;
}

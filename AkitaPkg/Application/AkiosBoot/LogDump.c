/** @file
  AkiosBoot: the log. The phone has no serial console, so what the boot application and UniXNU say goes to the
  start of the logdump partition (plain text, as xnuloader wrote it), to be read from Android afterwards.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AkiosBoot.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>

#define LOG_SIZE  SIZE_256KB

STATIC CHAR8                   mLog[LOG_SIZE];
STATIC UINTN                   mLength;
STATIC EFI_BLOCK_IO_PROTOCOL   *mPartition;
STATIC BOOLEAN                 mLooked;
STATIC VOID                    *mOut;           // block-aligned copy for the write (U-Boot wants io_align)
STATIC UINTN                   mOutPages;
STATIC BOOLEAN                 mWriteFailed;

//
// The console's own OutputString (AkiosBoot.c keeps it before wrapping the console), for the echo: U-Boot's StdErr
// is the console, and EDK2's debug channel prints nothing at UniXNU's level there.
//
EFI_TEXT_STRING  gAkiosConsoleOutput;

VOID
AkiosLogEcho (
  IN CONST CHAR8  *Line
  )
{
  CHAR16  Wide[256];

  if ((gAkiosConsoleOutput == NULL) || (gST->ConOut == NULL)) {
    return;
  }

  UnicodeSPrintAsciiFormat (Wide, sizeof (Wide), "%a", Line);
  gAkiosConsoleOutput (gST->ConOut, Wide);
}

VOID
EFIAPI
AkiosLogLine (
  IN CONST CHAR8  *Line,
  IN UINTN        Length
  )
{
  if (Length > LOG_SIZE - 1 - mLength) {
    Length = LOG_SIZE - 1 - mLength;      // the start of a boot is what matters: a full log keeps its beginning
  }

  CopyMem (mLog + mLength, Line, Length);
  mLength += Length;
  mLog[mLength] = 0;
}

VOID
AkiosLog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Line[256];
  UINTN    Prefix;

  Prefix = AsciiSPrint (Line, sizeof (Line), "AKIOS: ");
  VA_START (Args, Format);
  AsciiVSPrint (Line + Prefix, sizeof (Line) - Prefix - 2, Format, Args);
  VA_END (Args);
  AsciiStrCatS (Line, sizeof (Line), "\r\n");
  AkiosLogLine (Line, AsciiStrLen (Line));
  AkiosLogEcho (Line);
}

STATIC
EFI_BLOCK_IO_PROTOCOL *
FindLogdump (
  VOID
  )
{
  STATIC CONST CHAR16          Want[] = L"logdump";
  EFI_HANDLE                   *Handles;
  UINTN                        Count;
  UINTN                        Index;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;

  BlockIo = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles))) {
    return NULL;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        (Info->Type != PARTITION_TYPE_GPT) ||
        (StrnCmp (Info->Info.Gpt.PartitionName, Want, ARRAY_SIZE (Want)) != 0))
    {
      continue;
    }

    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo))) {
      break;
    }

    BlockIo = NULL;
  }

  FreePool (Handles);
  return BlockIo;
}

VOID
AkiosLogFlush (
  VOID
  )
{
  UINTN       Block;
  UINTN       Bytes;
  EFI_STATUS  Status;

  //
  // Looked for again at every flush until found: the first lines come before UniXNU connects the controllers,
  // and on a firmware that connects only the boot device the partition appears then.
  //
  if (mPartition == NULL) {
    mPartition = FindLogdump ();
    if (mPartition == NULL) {
      if (!mLooked) {
        mLooked = TRUE;
        AkiosLog ("log: no logdump partition yet: kept in memory until there is one");
      }

      return;
    } else {
      AkiosLog ("log: logdump found, %u-byte blocks, %lu of them", mPartition->Media->BlockSize,
        (UINT64)mPartition->Media->LastBlock + 1);
      mOutPages = EFI_SIZE_TO_PAGES (LOG_SIZE + SIZE_64KB);
      mOut      = AllocatePages (mOutPages);       // page-aligned: enough for any block size up to 4 KB
      if (mOut == NULL) {
        mPartition = NULL;
      }
    }
  }

  if (mPartition == NULL) {
    return;
  }

  Block = mPartition->Media->BlockSize;
  Bytes = ALIGN_VALUE (mLength + 1, Block);
  if ((Block == 0) || (Bytes > EFI_PAGES_TO_SIZE (mOutPages)) ||
      ((Bytes / Block) > mPartition->Media->LastBlock + 1))
  {
    return;
  }

  ZeroMem (mOut, Bytes);
  CopyMem (mOut, mLog, mLength);
  Status = mPartition->WriteBlocks (mPartition, mPartition->Media->MediaId, 0, Bytes, mOut);
  if (EFI_ERROR (Status) && !mWriteFailed) {
    mWriteFailed = TRUE;
    AkiosLog ("log: writing %u bytes to logdump: %r", (UINT32)Bytes, Status);
  }

  mPartition->FlushBlocks (mPartition);
}

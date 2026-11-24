/** @file
  AkiosBoot: the boot payload U-Boot copied to AKIOS_PAYLOAD_BASE.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AkiosBoot.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

typedef struct {
  UINT32  Magic;
  UINT32  Size;
} PAYLOAD_HEADER;

typedef struct {
  CHAR8   Name[16];
  UINT32  Size;
  UINT32  Reserved;
} PAYLOAD_ENTRY;

//
// Calls FOUND for each entry, in order, until it returns TRUE; the number of entries walked. The payload's own
// size bounds every read: a damaged one ends the walk, never reads past it.
//
STATIC
UINTN
Walk (
  IN BOOLEAN (*Found)(CONST PAYLOAD_ENTRY *Entry, CONST UINT8 *Data, VOID *Context),
  IN VOID    *Context
  )
{
  CONST PAYLOAD_HEADER  *Header;
  CONST UINT8           *At;
  CONST UINT8           *End;
  CONST PAYLOAD_ENTRY   *Entry;
  UINTN                 Count;

  Header = (CONST PAYLOAD_HEADER *)(UINTN)AKIOS_PAYLOAD_BASE;
  if ((Header->Magic != AKIOS_PAYLOAD_MAGIC) || (Header->Size > AKIOS_PAYLOAD_MAX - sizeof (*Header))) {
    return 0;
  }

  At    = (CONST UINT8 *)(Header + 1);
  End   = At + Header->Size;
  Count = 0;
  while ((UINTN)(End - At) >= sizeof (PAYLOAD_ENTRY)) {
    Entry = (CONST PAYLOAD_ENTRY *)At;
    if ((Entry->Name[0] == 0) || (Entry->Size > (UINTN)(End - At) - sizeof (*Entry))) {
      break;
    }

    Count++;
    if ((Found != NULL) && Found (Entry, At + sizeof (*Entry), Context)) {
      break;
    }

    At += sizeof (*Entry) + ALIGN_VALUE (Entry->Size, 16);
  }

  return Count;
}

typedef struct {
  CONST CHAR8  *Name;
  AKIOS_ITEM   *Item;
  BOOLEAN      Found;
} FIND;

STATIC
BOOLEAN
FindOne (
  IN CONST PAYLOAD_ENTRY  *Entry,
  IN CONST UINT8          *Data,
  IN VOID                 *Context
  )
{
  FIND  *Find;

  Find = Context;
  if (AsciiStrnCmp (Entry->Name, Find->Name, sizeof (Entry->Name)) != 0) {
    return FALSE;
  }

  Find->Item->Data = Data;
  Find->Item->Size = Entry->Size;
  Find->Found      = TRUE;
  return TRUE;
}

BOOLEAN
AkiosPayloadItem (
  IN  CONST CHAR8  *Name,
  OUT AKIOS_ITEM   *Item
  )
{
  FIND  Find;

  Find.Name  = Name;
  Find.Item  = Item;
  Find.Found = FALSE;
  Walk (FindOne, &Find);
  return Find.Found;
}

STATIC
BOOLEAN
LogOne (
  IN CONST PAYLOAD_ENTRY  *Entry,
  IN CONST UINT8          *Data,
  IN VOID                 *Context
  )
{
  CHAR8  Name[sizeof (Entry->Name) + 1];

  (VOID)Context;
  CopyMem (Name, Entry->Name, sizeof (Entry->Name));
  Name[sizeof (Entry->Name)] = 0;
  AkiosLog ("payload: %a, %u bytes at 0x%lx", Name, Entry->Size, (UINT64)(UINTN)Data);
  return FALSE;
}

UINTN
AkiosPayloadSurvey (
  VOID
  )
{
  return Walk (LogOne, NULL);
}

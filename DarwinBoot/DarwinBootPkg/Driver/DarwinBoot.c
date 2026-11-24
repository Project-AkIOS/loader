// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Guid/HobList.h>
#include <Protocol/UniXNULoader.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SerialIo.h>
#include "BootArgs.h"
#include "BootVideo.h"
#include "PackedRamdisk.h"
#include "WindowSize.h"
#include "MemoryWindow.h"
#include "LoaderInputs.h"

#define WINDOW_SIZE  SIZE_256MB
// The kernel's physical and virtual addresses agree modulo this: XNU's first page tables (start.s) map the kernel
// with level 2 blocks, 2 MiB with 4 KiB pages and 32 MiB with the 16 KiB pages of an arm64 iOS kernel.
#define BLOCK_SIZE   SIZE_32MB
// A window that cannot have the size the device tree asks for may be smaller, down to this.
#define WINDOW_MIN   SIZE_1GB
// The largest kernel file read: the default window. A macOS kernel collection is about 137 MiB; the window check
// below still bounds the kernel with everything placed after it.
#define KERNEL_LIMIT SIZE_256MB
#define MAX_SEGMENTS 64

typedef struct {
  UINT64  Address;
  UINT64  Size;
  UINT64  FileOffset;
  UINT64  FileSize;
  UINT32  Protection;
} SEGMENT;

typedef struct {
  SEGMENT  Segments[MAX_SEGMENTS];
  UINTN    Count;
  UINT64   Base;
  UINT64   End;
  UINT64   Entry;
} KERNEL;

// Where the parts of the loaded image go, and the bytes they take from the start of the window.
typedef struct {
  UINT64  Args;
  UINT64  Tree;
  UINT64  Ramdisk;
  UINT64  TrustCache;
  UINT64  Addon;
  UINT64  Policy;
  UINT64  Used;
} LAYOUT;

STATIC EFI_HANDLE  mImage;
UINT64 EFIAPI DarwinCurrentEl (VOID);
UINT64 EFIAPI DarwinMpidr (VOID);
UINT64 EFIAPI DarwinCounter (VOID);
VOID EFIAPI DarwinEnter (UINT64 Args, UINT64 Entry, UINT64 Memory, UINT64 Size, UINT64 VideoBase, UINT64 VideoBytes,
                         UINT64 El2Vectors);
VOID EFIAPI DarwinFlush (UINT64 Memory, UINT64 Size);
// Entry.S: the EL2 code the kernel's cores keep (copied out of the image), where its vectors lie in it, its size.
extern UINT8   DarwinEl2Trampoline[];
extern UINT64  DarwinEl2VectorsOffset;
extern UINT64  DarwinEl2TrampolineSize;

#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })

STATIC UINT32 U32 (CONST VOID *P) { return ReadUnaligned32 ((CONST UINT32 *)P); }
STATIC UINT64 U64 (CONST VOID *P) { return ReadUnaligned64 ((CONST UINT64 *)P); }

STATIC EFI_SERIAL_IO_PROTOCOL  *mSerial;

// Print, and the same text to the firmware's serial port: a phone has no screen anyone reads at boot, and its
// firmware's serial port is the log it keeps over a restart (akita: the pstore zone Android shows).
STATIC
VOID
EFIAPI
DarwinLog (IN CONST CHAR16 *Format, ...)
{
  VA_LIST  Marker;
  CHAR16   Line[640];
  CHAR8    Ascii[640];
  UINTN    Index;
  UINTN    Size;

  VA_START (Marker, Format);
  UnicodeVSPrint (Line, sizeof (Line), Format, Marker);
  VA_END (Marker);
  if (gST->ConOut != NULL) { gST->ConOut->OutputString (gST->ConOut, Line); }
  if ((mSerial == NULL) && EFI_ERROR (gBS->LocateProtocol (&gEfiSerialIoProtocolGuid, NULL, (VOID **)&mSerial))) {
    mSerial = NULL;
  }
  if (mSerial == NULL) { return; }
  for (Index = 0; (Line[Index] != 0) && (Index < ARRAY_SIZE (Ascii) - 1); Index++) {
    Ascii[Index] = (Line[Index] < 0x80) ? (CHAR8)Line[Index] : '?';
  }
  Size = Index;
  mSerial->Write (mSerial, &Size, Ascii);
}

STATIC
EFI_STATUS
ReadBootVideo (UINT64 Video[6])
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL           *Gop;
  EFI_STATUS                            Status;

  ZeroMem (Video, 6 * sizeof (*Video));
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  if (Status == EFI_NOT_FOUND) { return EFI_SUCCESS; }
  if (EFI_ERROR (Status)) { return Status; }
  return DarwinBootVideoFromGop (Gop, Video);
}

STATIC
EFI_STATUS
ReadFile (
  EFI_FILE_PROTOCOL *Root,
  CONST CHAR16      *Path,
  UINTN             Limit,
  VOID              **Data,
  UINTN             *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              Actual;

  *Data = NULL;
  *Size = 0;
  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Info = NULL;
  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) { goto Done; }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) { goto Done; }
  if ((Info->Attribute & EFI_FILE_DIRECTORY) || (Info->FileSize == 0) || (Info->FileSize > Limit)) {
    DarwinLog (L"DARWINBOOT: %s attr %lx size %lu limit %lu\n", Path, (UINT64)Info->Attribute, (UINT64)Info->FileSize, (UINT64)Limit);
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  *Size = (UINTN)Info->FileSize;
  *Data = AllocatePool (*Size);
  if (*Data == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Actual = *Size;
  Status = File->Read (File, &Actual, *Data);
  if (!EFI_ERROR (Status) && (Actual != *Size)) { Status = EFI_DEVICE_ERROR; }
Done:
  if (Info != NULL) { FreePool (Info); }
  File->Close (File);
  if (EFI_ERROR (Status) && (*Data != NULL)) { FreePool (*Data); *Data = NULL; }
  return Status;
}

STATIC
EFI_STATUS
ParseKernelHeader (CONST UINT8 *Data, UINTN Size, UINTN Header, KERNEL *Kernel)
{
  UINTN    Offset;
  UINTN    End;
  UINTN    Index;
  UINTN    Other;
  UINT32   Count;
  UINT32   Command;
  UINT32   Length;
  SEGMENT  *Segment;
  BOOLEAN  HasEntry;
  BOOLEAN  Executable;

  if ((Header > Size) || (Size - Header < 32) ||
      (U32 (Data + Header) != 0xfeedfacf) || (U32 (Data + Header + 4) != 0x100000c) ||
      (((U32 (Data + Header + 8) & 0x00ffffff) != 0) &&
       ((U32 (Data + Header + 8) & 0x00ffffff) != 2)) ||
      ((U32 (Data + Header + 12) != 2) &&
       ((Header != 0) || (U32 (Data + Header + 12) != 12)))) {
    return EFI_UNSUPPORTED;
  }
  if (U32 (Data + Header + 20) > Size - Header - 32) { return EFI_LOAD_ERROR; }
  ZeroMem (Kernel, sizeof (*Kernel));
  Count = U32 (Data + Header + 16);
  End = Header + 32 + U32 (Data + Header + 20);
  Offset = Header + 32;
  HasEntry = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (End - Offset < 8) { return EFI_LOAD_ERROR; }
    Command = U32 (Data + Offset);
    Length = U32 (Data + Offset + 4);
    if ((Length < 8) || (Length & 7) || (Length > End - Offset)) { return EFI_LOAD_ERROR; }
    if (Command == 0x19) {
      if ((Length < 72) || (U32 (Data + Offset + 64) > (Length - 72) / 80) ||
          (Kernel->Count == MAX_SEGMENTS)) { return EFI_LOAD_ERROR; }
      if (U64 (Data + Offset + 32) != 0) {
        Segment = &Kernel->Segments[Kernel->Count++];
        Segment->Address = U64 (Data + Offset + 24);
        Segment->Size = U64 (Data + Offset + 32);
        Segment->FileOffset = U64 (Data + Offset + 40);
        Segment->FileSize = U64 (Data + Offset + 48);
        Segment->Protection = U32 (Data + Offset + 60);
        if ((Segment->Size > WINDOW_SIZE) || (Segment->Address > MAX_UINT64 - Segment->Size) ||
            (Segment->FileSize > Segment->Size) || (Segment->FileOffset > Size) ||
            (Segment->FileSize > Size - Segment->FileOffset)) { return EFI_LOAD_ERROR; }
      }
    } else if (Command == 5) {
      if (HasEntry || (Length != 288) || (U32 (Data + Offset + 8) != 6) ||
          (U32 (Data + Offset + 12) != 68)) { return EFI_LOAD_ERROR; }
      Kernel->Entry = U64 (Data + Offset + 272);
      HasEntry = TRUE;
    }
    Offset += Length;
  }
  if ((Offset != End) || !HasEntry || (Kernel->Count == 0)) { return EFI_LOAD_ERROR; }
  Kernel->Base = MAX_UINT64;
  Executable = FALSE;
  for (Index = 0; Index < Kernel->Count; Index++) {
    Segment = &Kernel->Segments[Index];
    Kernel->Base = MIN (Kernel->Base, Segment->Address);
    Kernel->End = MAX (Kernel->End, Segment->Address + Segment->Size);
    if ((Kernel->Entry >= Segment->Address) && (Kernel->Entry - Segment->Address < Segment->FileSize) &&
        ((Segment->Protection & 4) != 0)) { Executable = TRUE; }
    for (Other = 0; Other < Index; Other++) {
      if ((Segment->Address < Kernel->Segments[Other].Address + Kernel->Segments[Other].Size) &&
          (Kernel->Segments[Other].Address < Segment->Address + Segment->Size)) { return EFI_LOAD_ERROR; }
    }
  }
  if (!Executable || (Kernel->Entry & 3) || (Kernel->Base & EFI_PAGE_MASK) ||
      (Kernel->Base > MAX_UINT64 - WINDOW_SIZE) ||
      (Kernel->End - Kernel->Base > WINDOW_SIZE - SIZE_2MB)) { return EFI_LOAD_ERROR; }
  return EFI_SUCCESS;
}

// Fileset segment offsets are absolute in the collection. XNU rebases and
// signs its own fixup chains after entry; firmware only copies root mappings.
STATIC
EFI_STATUS
ParseKernel (CONST UINT8 *Data, UINTN Size, KERNEL *Kernel)
{
  EFI_STATUS Status;
  KERNEL     Member;
  UINTN      Offset;
  UINTN      End;
  UINTN      Header;
  UINTN      Name;
  UINTN      Index;
  UINTN      Other;
  UINT32     Length;
  BOOLEAN    Found;
  BOOLEAN    Covered;
  SEGMENT    *Child;
  SEGMENT    *Parent;

  Status = ParseKernelHeader (Data, Size, 0, Kernel);
  if (EFI_ERROR (Status) || (U32 (Data + 12) != 12)) { return Status; }
  Offset = 32;
  End = 32 + U32 (Data + 20);
  Found = FALSE;
  while (Offset < End) {
    Length = U32 (Data + Offset + 4);
    if (U32 (Data + Offset) == 0x80000035) {
      if (Length < 40) { return EFI_LOAD_ERROR; }
      Name = U32 (Data + Offset + 24);
      if ((Name < 32) || (Name >= Length)) { return EFI_LOAD_ERROR; }
      for (Index = Name; (Index < Length) && (Data[Offset + Index] != 0); Index++) {}
      if (Index == Length) { return EFI_LOAD_ERROR; }
      Header = U64 (Data + Offset + 16);
      if ((Header > Size) || (Size - Header < 32) || (Header & 7)) { return EFI_LOAD_ERROR; }
      // Every indexed header must actually be loaded at its advertised VA.
      Covered = FALSE;
      for (Other = 0; Other < Kernel->Count; Other++) {
        Parent = &Kernel->Segments[Other];
        if ((Header >= Parent->FileOffset) &&
            (Header - Parent->FileOffset <= Parent->FileSize) &&
            (32 <= Parent->FileSize - (Header - Parent->FileOffset)) &&
            (U64 (Data + Offset + 8) == Parent->Address + Header - Parent->FileOffset)) {
          Covered = TRUE;
        }
      }
      if (!Covered) { return EFI_LOAD_ERROR; }
      if ((Index - Name == sizeof ("com.apple.kernel") - 1) &&
          (CompareMem (Data + Offset + Name, "com.apple.kernel", sizeof ("com.apple.kernel")) == 0)) {
        if (Found || (U32 (Data + Header + 12) != 2) ||
            (U32 (Data + Header + 8) != U32 (Data + 8)) ||
            ((U32 (Data + Header + 24) & 0x80000000) == 0)) { return EFI_LOAD_ERROR; }
        Found = TRUE;
        Status = ParseKernelHeader (Data, Size, Header, &Member);
        if (EFI_ERROR (Status) || (Member.Entry != Kernel->Entry)) { return EFI_LOAD_ERROR; }
        for (Index = 0; Index < Member.Count; Index++) {
          Child = &Member.Segments[Index];
          Covered = FALSE;
          for (Other = 0; Other < Kernel->Count; Other++) {
            Parent = &Kernel->Segments[Other];
            if ((Child->Address >= Parent->Address) &&
                (Child->Address - Parent->Address <= Parent->Size) &&
                (Child->Size <= Parent->Size - (Child->Address - Parent->Address)) &&
                ((Child->Protection & ~Parent->Protection) == 0) &&
                ((Child->FileSize == 0) ||
                 ((Child->Address - Parent->Address <= Parent->FileSize) &&
                  (Child->FileSize <= Parent->FileSize - (Child->Address - Parent->Address)) &&
                  (Child->FileOffset == Parent->FileOffset + Child->Address - Parent->Address)))) {
              Covered = TRUE;
            }
          }
          if (!Covered) { return EFI_LOAD_ERROR; }
        }
      }
    }
    Offset += Length;
  }
  return Found ? EFI_SUCCESS : EFI_LOAD_ERROR;
}

STATIC
BOOLEAN
AdtNode (CONST UINT8 *Data, UINTN Size, UINTN *Offset, UINTN Depth, UINTN ParentPath, UINTN *RamdiskOffset, UINTN *TrustCacheOffset, UINTN *AddonOffset, UINTN *PolicyOffset, UINT64 *WindowSize, BOOLEAN *KeepPacked, UINTN *ExcludedOffset, UINTN *ExcludedCapacity, UINTN *InputsOffset)
{
  UINT32 Properties;
  UINT32 Children;
  UINT32 Length;
  UINTN  Index;
  UINTN  Path;
  UINTN  Candidate;
  UINTN  TrustCandidate;
  UINTN  AddonCandidate;
  UINTN  PolicyCandidate;
  UINTN  MemoryCandidate;
  UINTN  ExcludedCandidate;
  UINTN  ExcludedLength;
  UINTN  InputsCandidate;
  CONST UINT8 *Name;
  UINTN  NameLength;

  Name = NULL;
  NameLength = 0;
  Candidate = MAX_UINTN;
  TrustCandidate = MAX_UINTN;
  AddonCandidate = MAX_UINTN;
  PolicyCandidate = MAX_UINTN;
  MemoryCandidate = MAX_UINTN;
  ExcludedCandidate = MAX_UINTN;
  ExcludedLength = 0;
  InputsCandidate = MAX_UINTN;
  if ((Depth > 64) || (Size - *Offset < 8)) { return FALSE; }
  Properties = U32 (Data + *Offset);
  Children = U32 (Data + *Offset + 4);
  *Offset += 8;
  if (Properties > (Size - *Offset) / 36) { return FALSE; }
  for (Index = 0; Index < Properties; Index++) {
    if (Size - *Offset < 36) { return FALSE; }
    if (ScanMem8 (Data + *Offset, 32, 0) == NULL) { return FALSE; }
    Length = U32 (Data + *Offset + 32);
    *Offset += 36;
    if (Length > Size - *Offset) { return FALSE; }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "name") == 0) {
      if (Name != NULL) { return FALSE; }
      Name = Data + *Offset;
      NameLength = Length;
    }
    if ((AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RAMDisk") == 0) ||
        (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0)) {
      if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0) {
        if (*KeepPacked) { return FALSE; }
        *KeepPacked = TRUE;
      }
      if ((Candidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      Candidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "TrustCache") == 0) {
      if ((TrustCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      TrustCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "GpuAddon") == 0) {
      if ((AddonCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      AddonCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RootPolicies") == 0) {
      if ((PolicyCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      PolicyCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-size") == 0) {
      if ((MemoryCandidate != MAX_UINTN) || (Length != 8)) { return FALSE; }
      MemoryCandidate = *Offset;
    }
    // The table the loader fills with the ranges of the window XNU must not use (MemoryWindow.h): empty pairs.
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-excluded") == 0) {
      if ((ExcludedCandidate != MAX_UINTN) || (Length < 16) || (Length & 15) ||
          !IsZeroBuffer (Data + *Offset, Length)) { return FALSE; }
      ExcludedCandidate = *Offset;
      ExcludedLength = Length;
    }
    // The records the loader fills with the memory of its inputs (LoaderInputs.h): empty.
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-loader-inputs") == 0) {
      if ((InputsCandidate != MAX_UINTN) || (Length != DARWIN_INPUT_COUNT * DARWIN_INPUT_RECORD_SIZE) ||
          !IsZeroBuffer (Data + *Offset, Length)) { return FALSE; }
      InputsCandidate = *Offset;
    }
    *Offset += Length;
    if (ALIGN_VALUE (*Offset, 4) > Size) { return FALSE; }
    *Offset = ALIGN_VALUE (*Offset, 4);
  }
  Path = 3;
  if (Depth == 0) { Path = 0; }
  if ((Depth == 1) && (ParentPath == 0) && (NameLength == 7) && (CompareMem (Name, "chosen", 7) == 0)) { Path = 1; }
  if ((Depth == 2) && (ParentPath == 1) && (NameLength == 11) && (CompareMem (Name, "memory-map", 11) == 0)) { Path = 2; }
  if ((Path == 2) && (Candidate != MAX_UINTN)) {
    if (*RamdiskOffset != MAX_UINTN) { return FALSE; }
    *RamdiskOffset = Candidate;
  }
  if ((Path == 2) && (TrustCandidate != MAX_UINTN)) {
    if (*TrustCacheOffset != MAX_UINTN) { return FALSE; }
    *TrustCacheOffset = TrustCandidate;
  }
  if ((Path == 2) && (AddonCandidate != MAX_UINTN)) {
    if (*AddonOffset != MAX_UINTN) { return FALSE; }
    *AddonOffset = AddonCandidate;
  }
  if (PolicyCandidate != MAX_UINTN) {
    if ((Path != 2) || (*PolicyOffset != MAX_UINTN)) { return FALSE; }
    *PolicyOffset = PolicyCandidate;
  }
  if (MemoryCandidate != MAX_UINTN) {
    if ((Path != 1) || (*WindowSize != 0)) { return FALSE; }
    *WindowSize = U64 (Data + MemoryCandidate);
    if (!DarwinWindowSizeValid (*WindowSize)) { return FALSE; }
  }
  if (ExcludedCandidate != MAX_UINTN) {
    if ((Path != 1) || (*ExcludedOffset != MAX_UINTN)) { return FALSE; }
    *ExcludedOffset = ExcludedCandidate;
    *ExcludedCapacity = ExcludedLength / 16;
  }
  if (InputsCandidate != MAX_UINTN) {
    if ((Path != 1) || (*InputsOffset != MAX_UINTN)) { return FALSE; }
    *InputsOffset = InputsCandidate;
  }
  if (Children > (Size - *Offset) / 8) { return FALSE; }
  for (Index = 0; Index < Children; Index++) {
    if (!AdtNode (Data, Size, Offset, Depth + 1, Path, RamdiskOffset, TrustCacheOffset, AddonOffset, PolicyOffset, WindowSize, KeepPacked, ExcludedOffset, ExcludedCapacity, InputsOffset)) { return FALSE; }
  }
  return TRUE;
}

STATIC
BOOLEAN
ValidTrustCaches (CONST UINT8 *Data, UINTN Size)
{
  UINTN Count;
  UINTN Index;
  UINTN Start;
  UINTN End;
  UINTN Entries;
  UINTN Entry;

  if (Size < 8) { return FALSE; }
  Count = U32 (Data);
  if ((Count == 0) || (Count > 128) || (Count > (Size - 4) / 4)) { return FALSE; }
  for (Index = 0; Index < Count; Index++) {
    Start = U32 (Data + 4 + Index * 4);
    End = (Index + 1 < Count) ? U32 (Data + 8 + Index * 4) : Size;
    if ((Start < 4 + Count * 4) || (Start & 3) || (End > Size) ||
        (End < Start) || (End - Start < 24) || (U32 (Data + Start) != 2)) { return FALSE; }
    Entries = U32 (Data + Start + 20);
    if ((Entries > (End - Start - 24) / 24) || (End - Start != 24 + Entries * 24)) { return FALSE; }
    for (Entry = 0; Entry < Entries; Entry++) {
      CONST UINT8 *Hash = Data + Start + 24 + Entry * 24;
      if ((Hash[20] < 1) || (Hash[20] > 4) ||
          ((Entry != 0) && (CompareMem (Hash - 24, Hash, 20) >= 0))) { return FALSE; }
    }
  }
  return TRUE;
}

// The device tree as AdtNode() accepted it: the offset just past the node at Offset.
STATIC
BOOLEAN
AdtNodeEnd (CONST UINT8 *Data, UINTN Size, UINTN Offset, UINTN Depth, UINTN *End)
{
  UINT32 Properties;
  UINT32 Children;
  UINT32 Length;
  UINTN  Index;

  if ((Depth > 64) || (Offset > Size) || (Size - Offset < 8)) { return FALSE; }
  Properties = U32 (Data + Offset);
  Children = U32 (Data + Offset + 4);
  Offset += 8;
  for (Index = 0; Index < Properties; Index++) {
    if (Size - Offset < 36) { return FALSE; }
    Length = U32 (Data + Offset + 32);
    Offset += 36;
    if (Length > Size - Offset) { return FALSE; }
    Offset = ALIGN_VALUE (Offset + Length, 4);
    if (Offset > Size) { return FALSE; }
  }
  for (Index = 0; Index < Children; Index++) {
    if (!AdtNodeEnd (Data, Size, Offset, Depth + 1, &Offset)) { return FALSE; }
  }
  *End = Offset;
  return TRUE;
}

// The value of property Name of the node at Node: its offset in the tree and its length.
STATIC
BOOLEAN
AdtProperty (CONST UINT8 *Data, UINTN Size, UINTN Node, CONST CHAR8 *Name, UINTN *Value, UINT32 *Length)
{
  UINT32 Properties;
  UINT32 Index;
  UINTN  Offset;

  if ((Node > Size) || (Size - Node < 8)) { return FALSE; }
  Properties = U32 (Data + Node);
  Offset = Node + 8;
  for (Index = 0; Index < Properties; Index++) {
    if (Size - Offset < 36) { return FALSE; }
    *Length = U32 (Data + Offset + 32);
    if (*Length > Size - Offset - 36) { return FALSE; }
    if ((ScanMem8 (Data + Offset, 32, 0) != NULL) && (AsciiStrCmp ((CONST CHAR8 *)(Data + Offset), Name) == 0)) {
      *Value = Offset + 36;
      return TRUE;
    }
    Offset = ALIGN_VALUE (Offset + 36 + *Length, 4);
  }
  return FALSE;
}

// The child of the node at Node whose name is Name.
STATIC
BOOLEAN
AdtChild (CONST UINT8 *Data, UINTN Size, UINTN Node, CONST CHAR8 *Name, UINTN *Child)
{
  UINT32 Children;
  UINT32 Index;
  UINT32 Length;
  UINTN  Offset;
  UINTN  Value;
  UINTN  NameLength;

  if ((Node > Size) || (Size - Node < 8)) { return FALSE; }
  Children = U32 (Data + Node + 4);
  // past the node's own properties: the end of a node without its children
  Offset = Node + 8;
  for (Index = 0; Index < U32 (Data + Node); Index++) {
    if (Size - Offset < 36) { return FALSE; }
    Length = U32 (Data + Offset + 32);
    if (Length > Size - Offset - 36) { return FALSE; }
    Offset = ALIGN_VALUE (Offset + 36 + Length, 4);
  }
  NameLength = AsciiStrSize (Name);
  for (Index = 0; Index < Children; Index++) {
    if (AdtProperty (Data, Size, Offset, "name", &Value, &Length) && (Length >= NameLength) &&
        (CompareMem (Data + Value, Name, NameLength) == 0)) {
      *Child = Offset;
      return TRUE;
    }
    if (!AdtNodeEnd (Data, Size, Offset, 1, &Offset)) { return FALSE; }
  }
  return FALSE;
}

// Property Name of /chosen, its offset and length.
STATIC
BOOLEAN
AdtChosen (CONST UINT8 *Data, UINTN Size, CONST CHAR8 *Name, UINTN *Value, UINT32 *Length)
{
  UINTN Chosen;

  return AdtChild (Data, Size, 0, "chosen", &Chosen) && AdtProperty (Data, Size, Chosen, Name, Value, Length);
}

// Seconds since 1970 by the firmware's clock, or 0 when it has none.
STATIC
UINT64
FirmwareEpoch (VOID)
{
  STATIC CONST UINT16  BeforeMonth[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  EFI_TIME             Time;
  UINT64               Days;
  UINT64               Seconds;

  if ((gST->RuntimeServices == NULL) || EFI_ERROR (gST->RuntimeServices->GetTime (&Time, NULL)) ||
      (Time.Year < 2024) || (Time.Year > 2099) || (Time.Month < 1) || (Time.Month > 12)) { return 0; }
  Days = (UINT64)(Time.Year - 1970) * 365 + (Time.Year - 1969) / 4 + BeforeMonth[Time.Month - 1] + (Time.Day - 1);
  if ((Time.Month > 2) && ((Time.Year % 4) == 0)) { Days++; }
  Seconds = Days * 86400 + (UINT64)Time.Hour * 3600 + (UINT64)Time.Minute * 60 + Time.Second;
  if (Time.TimeZone != EFI_UNSPECIFIED_TIMEZONE) { Seconds += (INT64)Time.TimeZone * 60; }
  return Seconds;
}

// The kernel starts on the core that runs this and takes the first of /cpus (cpu0, cpu1, ...) for it: the core
// whose "reg" (MPIDR affinity) is this core's changes places with cpu0 (only the reg values move); a list that
// does not have this core at all gets it as cpu0.
STATIC
VOID
BootCoreFirst (UINT8 *Tree, UINTN TreeSize)
{
  UINTN   Cpus;
  UINTN   Node;
  UINTN   Value;
  UINTN   First;
  UINT32  Length;
  UINT32  Self;
  UINT32  Index;
  CHAR8   Name[8];

  Self = (UINT32)(DarwinMpidr () & 0xFFFFFF);
  if (!AdtChild (Tree, TreeSize, 0, "cpus", &Cpus) || !AdtChild (Tree, TreeSize, Cpus, "cpu0", &Node) ||
      !AdtProperty (Tree, TreeSize, Node, "reg", &First, &Length) || (Length != 4) || (U32 (Tree + First) == Self)) {
    return;
  }
  for (Index = 1; Index < 64; Index++) {
    AsciiSPrint (Name, sizeof (Name), "cpu%u", Index);
    if (!AdtChild (Tree, TreeSize, Cpus, Name, &Node)) { break; }
    if (AdtProperty (Tree, TreeSize, Node, "reg", &Value, &Length) && (Length == 4) && (U32 (Tree + Value) == Self)) {
      WriteUnaligned32 ((UINT32 *)(Tree + Value), U32 (Tree + First));
      break;
    }
  }
  DarwinLog (L"DARWINBOOT: the boot core 0x%x goes first in /cpus\n", Self);
  WriteUnaligned32 ((UINT32 *)(Tree + First), Self);
}

// What only the boot can know, into the placeholders of /chosen that the tree on the ESP carries (all optional,
// all zero there): dram-base and dram-size (the window), el2-trampoline (where the kernel's cores enter at EL2),
// random-seed (any length) and boot-epoch (the firmware's clock).
STATIC
VOID
FillTree (UINT8 *Tree, UINTN TreeSize, UINT64 DramBase, UINT64 DramSize, UINT64 Trampoline)
{
  UINTN   Value;
  UINT32  Length;
  UINT32  Index;
  UINT64  Seed;
  UINT64  Mixed;

  if (AdtChosen (Tree, TreeSize, "dram-base", &Value, &Length) && (Length == 8)) { WriteUnaligned64 ((UINT64 *)(Tree + Value), DramBase); }
  if (AdtChosen (Tree, TreeSize, "dram-size", &Value, &Length) && (Length == 8)) { WriteUnaligned64 ((UINT64 *)(Tree + Value), DramSize); }
  if (AdtChosen (Tree, TreeSize, "el2-trampoline", &Value, &Length) && (Length == 8)) {
    WriteUnaligned64 ((UINT64 *)(Tree + Value), Trampoline);
  }
  if (AdtChosen (Tree, TreeSize, "boot-epoch", &Value, &Length) && (Length == 8)) {
    WriteUnaligned64 ((UINT64 *)(Tree + Value), FirmwareEpoch ());
  }
  BootCoreFirst (Tree, TreeSize);
  if (AdtChosen (Tree, TreeSize, "random-seed", &Value, &Length)) {
    // (splitmix64 over the counter, the time and where things landed: not a cryptographic source)
    Seed = DarwinCounter () ^ (FirmwareEpoch () << 20) ^ DramBase ^ (UINT64)(UINTN)Tree;
    for (Index = 0; Index < Length; Index++) {
      if ((Index & 7) == 0) {
        Seed += 0x9E3779B97F4A7C15ULL ^ DarwinCounter ();
        Mixed = (Seed ^ (Seed >> 30)) * 0xBF58476D1CE4E5B9ULL;
        Mixed = (Mixed ^ (Mixed >> 27)) * 0x94D049BB133111EBULL;
        Mixed ^= Mixed >> 31;
      }
      Tree[Value + Index] = (UINT8)(Mixed >> (8 * (Index & 7)));
    }
  }
}

STATIC
EFI_STATUS
GetMap (EFI_MEMORY_DESCRIPTOR **Map, UINTN *Capacity, UINTN *Size, UINTN *Key, UINTN *Stride)
{
  EFI_STATUS Status;
  UINT32     Version;
  UINTN      Attempt;

  for (Attempt = 0; Attempt < 3; Attempt++) {
    *Size = *Capacity;
    Status = gBS->GetMemoryMap (Size, *Map, Key, Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) { return Status; }
    if (*Map != NULL) { FreePool (*Map); }
    *Capacity = *Size + 32 * sizeof (EFI_MEMORY_DESCRIPTOR);
    *Map = AllocatePool (*Capacity);
    if (*Map == NULL) { return EFI_OUT_OF_RESOURCES; }
  }
  return EFI_OUT_OF_RESOURCES;
}

// End of the run of EfiConventionalMemory that begins at Start: the UEFI map often lists physically
// adjacent free ranges as separate descriptors, so extend over every descriptor that continues the run.
STATIC
UINT64
RunEnd (EFI_MEMORY_DESCRIPTOR *Map, UINTN Size, UINTN Stride, UINT64 Start, UINT64 End)
{
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  UINTN                 Offset;
  BOOLEAN               Extended;

  do {
    Extended = FALSE;
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->PhysicalStart == End) &&
          (Descriptor->NumberOfPages != 0) && (Descriptor->NumberOfPages < (MAX_UINT64 - End) / EFI_PAGE_SIZE)) {
        End += EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
        Extended = TRUE;
      }
    }
  } while (Extended);
  (VOID)Start;
  return End;
}

// Large diagnostic text straight into the GOP framebuffer: the UEFI text console font is too small to
// photograph. 5x12 glyphs from DiagFont.h scaled 4x, white on a black box; wraps at the right edge.
#include "DiagFont.h"
STATIC CONST UINT64 mDiagFont[] = GLYPH_FONT;

STATIC
VOID
DiagDraw (CONST CHAR16 *Text, UINTN Row)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  UINT64                       Video[6];
  UINTN                        Scale = 4, X = 0, Y, Index, Gx, Gy, Dx, Dy, Cell, Line;
  UINT32                       *Fb;
  UINT64                       Glyph, Data;
  CHAR16                       Ch;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) ||
      EFI_ERROR (DarwinBootVideoFromGop (Gop, Video))) { return; }
  Cell = 6 * Scale;
  Line = 14 * Scale;
  Y = Video[4] / 4 + Row * Line;
  Fb = (UINT32 *)(UINTN)Video[0];
  for (Index = 0; Text[Index] != 0; Index++) {
    Ch = Text[Index];
    if ((Ch < 32) || (Ch > 126)) { continue; }
    if (X + Cell > Video[3]) { X = 0; Y += Line; }
    if (Y + Line > Video[4]) { return; }
    Glyph = mDiagFont[Ch - 32];
    for (Gy = 0; Gy < 14 * Scale; Gy++) {
      for (Gx = 0; Gx < Cell; Gx++) {
        Fb[(Y + Gy) * (Video[2] / 4) + X + Gx] = 0xFF000000;
      }
    }
    for (Gy = 0; Gy < 12; Gy++) {
      Data = (Gy < 6) ? ((Glyph >> 32) >> (Gy * 5)) : ((Glyph & 0xFFFFFFFFULL) >> ((Gy - 6) * 5));
      for (Gx = 0; Gx < 5; Gx++) {
        if ((Data >> Gx) & 1) {
          for (Dy = 0; Dy < Scale; Dy++) {
            for (Dx = 0; Dx < Scale; Dx++) {
              Fb[(Y + Gy * Scale + Dy) * (Video[2] / 4) + X + Gx * Scale + Dx] = 0xFFFFFFFF;
            }
          }
        }
      }
    }
    X += Cell;
  }
}

// The window of a kernel that knows of no excluded ranges: WindowSize bytes of free memory in one run, starting
// on a BLOCK_SIZE boundary (the kernel goes at its link address's offset inside the first block, the boot
// arguments' physical and virtual bases are the window's start and that block's). When no run holds WindowSize
// bytes, the largest run does, down to WINDOW_MIN, and *WindowSize says what was taken.
STATIC
EFI_STATUS
AllocateWindow (KERNEL *Kernel, UINT64 *WindowSizeInOut, EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  EFI_STATUS            Status;
  UINTN                 Capacity;
  UINTN                 Size;
  UINTN                 Key;
  UINTN                 Stride;
  UINTN                 Offset;
  UINTN                 Pass;
  UINT64                End;
  UINT64                Candidate;
  UINT64                Room;
  UINT64                BestRoom;
  UINT64                WindowSize;

  (VOID)Kernel;
  *Address = 0;
  Map = NULL;
  Capacity = 0;
  BestRoom = 0;
  WindowSize = *WindowSizeInOut;
  Status = GetMap (&Map, &Capacity, &Size, &Key, &Stride);
  if (EFI_ERROR (Status)) { if (Map != NULL) { FreePool (Map); } return Status; }
  Status = EFI_OUT_OF_RESOURCES;
  for (Pass = 0; (Pass < 2) && EFI_ERROR (Status); Pass++) {
    if (Pass == 1) {
      if ((BestRoom < WINDOW_MIN) || (BestRoom >= WindowSize)) { break; }
      DarwinLog (L"DARWINBOOT: no free run of 0x%lx bytes; the window is the largest, 0x%lx bytes\n", WindowSize, BestRoom);
      WindowSize = BestRoom;
    }
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type != EfiConventionalMemory) || (Descriptor->NumberOfPages == 0)) { continue; }
      End = RunEnd (Map, Size, Stride, Descriptor->PhysicalStart,
                    Descriptor->PhysicalStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
      Candidate = ALIGN_VALUE (Descriptor->PhysicalStart, BLOCK_SIZE);
      if ((Candidate < Descriptor->PhysicalStart) || (End <= Candidate)) { continue; }
      Room = (End - Candidate) & ~((UINT64)BLOCK_SIZE - 1);
      if (Room > BestRoom) { BestRoom = Room; }
      if (Room < WindowSize) { continue; }
      Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);
      if (!EFI_ERROR (Status)) { *Address = Candidate; break; }
    }
  }
  if (EFI_ERROR (Status)) {
    CHAR16 Line[96];
    UINTN  Row = 1;
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx BIG RUN %lx", WindowSize, BestRoom);
    DiagDraw (Line, 0);
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->NumberOfPages >= 0x400) && (Row < 8)) {
        UnicodeSPrint (Line, sizeof (Line), L"FREE %lx + %lx", Descriptor->PhysicalStart, EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
        DarwinLog (L"DARWINBOOT: %s\n", Line);
        DiagDraw (Line, Row++);
      }
    }
  } else {
    *WindowSizeInOut = WindowSize;
  }
  FreePool (Map);
  return Status;
}

// The window of a kernel that accepts excluded ranges (MemoryWindow.h): the start with the most usable memory
// inside WindowSize bytes. Only the head is allocated. The rest of the window is whatever the final memory map
// leaves usable, so the firmware keeps the memory it needs until ExitBootServices.
STATIC
EFI_STATUS
AllocateSpan (KERNEL *Kernel, UINT64 WindowSize, UINT64 HeadSize, EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_STATUS            Status;
  UINTN                 Capacity;
  UINTN                 Size;
  UINTN                 Key;
  UINTN                 Stride;
  UINT64                Candidate;
  UINT64                Usable;
  UINT64                SkipUsable;
  UINT64                SkipAddress;
  CHAR16                Line[96];

  *Address = 0;
  Map = NULL;
  Capacity = 0;
  Status = GetMap (&Map, &Capacity, &Size, &Key, &Stride);
  if (EFI_ERROR (Status)) { if (Map != NULL) { FreePool (Map); } return Status; }
  SkipUsable = MAX_UINT64;
  SkipAddress = 0;
  for (;;) {
    Candidate = DarwinWindowStart (Map, Size, Stride, BLOCK_SIZE, Kernel->Base & (BLOCK_SIZE - 1), HeadSize, WindowSize,
                                   SkipUsable, SkipAddress, &Usable);
    if (Candidate == 0) { Status = EFI_OUT_OF_RESOURCES; break; }
    SkipUsable = Usable;
    SkipAddress = Candidate;
    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (HeadSize), &Candidate);
    if (!EFI_ERROR (Status)) { *Address = Candidate; break; }
  }
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx HEAD %lx", WindowSize, HeadSize);
    DiagDraw (Line, 0);
  }
  FreePool (Map);
  return Status;
}

// The image behind the kernel: boot arguments, device tree, RAM disk, trust caches, GPU addon, root policies.
// Every boundary is aligned to at most 16 KiB, so the bytes used are the same at every address that has the
// kernel's offset inside a 2 MiB block.
STATIC
VOID
PlaceImage (
  UINT64        Address,
  CONST KERNEL  *Kernel,
  UINTN         TreeSize,
  UINTN         RamdiskBytes,
  UINTN         TrustCacheSize,
  UINTN         AddonSize,
  UINTN         PolicySize,
  LAYOUT        *Layout
  )
{
  Layout->Args = ALIGN_VALUE (Address + Kernel->End - Kernel->Base, SIZE_16KB);
  Layout->Tree = Layout->Args + EFI_PAGE_SIZE;
  Layout->Ramdisk = ALIGN_VALUE (Layout->Tree + TreeSize, EFI_PAGE_SIZE);
  Layout->TrustCache = Layout->Ramdisk + RamdiskBytes;
  Layout->Addon = ALIGN_VALUE (Layout->TrustCache + TrustCacheSize, SIZE_16KB);
  Layout->Policy = ALIGN_VALUE (Layout->Addon + AddonSize, SIZE_16KB);
  Layout->Used = ALIGN_VALUE (Layout->Policy + PolicySize, SIZE_16KB) - Address;
}

// The table for free memory outside the window (MemoryWindow.h DarwinOutsideRanges): the property
// darwinboot-memory-outside of the flattened tree, whose value is empty (base, size) pairs. A property record is a
// 32-byte name, a 32-bit length and the value. FALSE when the tree has none or its value is not such a table.
STATIC
BOOLEAN
FindOutsideTable (CONST UINT8 *Tree, UINTN TreeSize, UINTN *TableOffset, UINTN *Capacity)
{
  STATIC CONST CHAR8  Name[32] = "darwinboot-memory-outside";
  UINTN               Offset;
  UINT32              Length;

  for (Offset = 0; Offset + 36 <= TreeSize; Offset += 4) {
    if (CompareMem (Tree + Offset, Name, sizeof (Name)) != 0) { continue; }
    Length = ReadUnaligned32 ((CONST UINT32 *)(Tree + Offset + 32)) & 0x7fffffff;
    if ((Length < 16) || (Length & 15) || (Length > TreeSize - Offset - 36) ||
        !IsZeroBuffer (Tree + Offset + 36, Length)) { return FALSE; }
    *TableOffset = Offset + 36;
    *Capacity = Length / 16;
    return TRUE;
  }
  return FALSE;
}

// Fill the device tree's table from a memory map and report the usable bytes as the memory size. FALSE when the
// head is no longer whole, which the head allocation rules out.
STATIC
BOOLEAN
DescribeWindow (
  CONST VOID        *Map,
  UINTN             MapSize,
  UINTN             Stride,
  UINT64            Address,
  UINT64            WindowSize,
  UINT64            HeadSize,
  UINT8             *Table,
  UINTN             Capacity,
  DARWIN_BOOT_ARGS  *Args,
  UINTN             *Count
  )
{
  UINT64 Excluded;

  if (DarwinRunEnd (Map, MapSize, Stride, FALSE, Address, Address + HeadSize) != Address + HeadSize) { return FALSE; }
  *Count = DarwinWindowExclusions (Map, MapSize, Stride, Address, WindowSize, Table, Capacity, &Excluded);
  Args->MemorySizeActual = WindowSize - Excluded;
  return TRUE;
}

// The entry argument the firmware's SEC phase keeps from the first-stage boot loader: the address of that
// loader's device tree, as a 64-bit value in this GUID extension HOB.
STATIC CONST EFI_GUID mEntryArgumentHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

// Where the loader's inputs lie (LoaderInputs.h). The initrd is the caller's buffer. The payload RAM disk is
// the range the first-stage loader's device tree names, rounded up to whole sectors as the firmware's block
// device publishes it; it counts as verified when the boot volume's block device has exactly that size and
// reads its first and last sector from that memory, which is touched only once the size matches.
STATIC
VOID
LocateInputs (CONST UNIXNU_LOADER_BOOT_REQUEST *Request, DARWIN_LOADER_INPUT *Inputs)
{
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  VOID                   *Hob;
  UINTN                  HobSize;
  UINT64                 Tree;
  UINT64                 Start;
  UINT64                 End;
  UINT64                 Size;
  UINT8                  Sector[512];

  ZeroMem (Inputs, DARWIN_INPUT_COUNT * sizeof (*Inputs));
  if ((Request->Ramdisk != NULL) && (Request->RamdiskSize != 0)) {
    Inputs[DARWIN_INPUT_INITRD].Base = (UINT64)(UINTN)Request->Ramdisk;
    Inputs[DARWIN_INPUT_INITRD].Size = Request->RamdiskSize;
    Inputs[DARWIN_INPUT_INITRD].State = DARWIN_INPUT_LOCATED | DARWIN_INPUT_VERIFIED;
  }
  Hob = NULL;
  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiHobListGuid, &Hob)) || (Hob == NULL)) { return; }
  HobSize = DarwinHobListSize (Hob, (UINT64)(UINTN)Hob, SIZE_16MB);
  if ((HobSize == 0) || !DarwinHobValue (Hob, HobSize, (CONST UINT8 *)&mEntryArgumentHobGuid, &Tree) ||
      (Tree == 0) || (Tree & 3) || !DarwinFdtInitrd ((CONST UINT8 *)(UINTN)Tree, &Start, &End)) { return; }
  Size = End - Start;
  if ((Size > MAX_UINT64 - 511) || (ALIGN_VALUE (Size, 512) > MAX_UINT64 - Start)) { return; }
  Size = ALIGN_VALUE (Size, 512);
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].Base = Start;
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].Size = Size;
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].State = DARWIN_INPUT_LOCATED;
  if (EFI_ERROR (gBS->HandleProtocol (Request->Volume, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
      (BlockIo->Media == NULL) || BlockIo->Media->LogicalPartition || !BlockIo->Media->MediaPresent ||
      (BlockIo->Media->BlockSize != sizeof (Sector)) || (BlockIo->Media->LastBlock != Size / sizeof (Sector) - 1)) { return; }
  if (EFI_ERROR (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, sizeof (Sector), Sector)) ||
      (CompareMem (Sector, (CONST VOID *)(UINTN)Start, sizeof (Sector)) != 0) ||
      EFI_ERROR (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, BlockIo->Media->LastBlock, sizeof (Sector), Sector)) ||
      (CompareMem (Sector, (CONST VOID *)(UINTN)(Start + Size - sizeof (Sector)), sizeof (Sector)) != 0)) { return; }
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].State |= DARWIN_INPUT_VERIFIED;
}

STATIC
EFI_STATUS
EFIAPI
Boot (
  IN UNIXNU_LOADER_PROTOCOL    *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST *Request
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL               *Root;
  VOID                            *Image;
  VOID                            *Tree;
  VOID                            *TrustCaches;
  VOID                            *Addon;
  UINTN                           ImageSize;
  UINTN                           TreeSize;
  UINTN                           TrustCacheSize;
  UINTN                           AddonSize;
  CHAR16                          *TreePath;
  UINTN                           Length;
  UINTN                           Index;
  UINTN                           Offset;
  KERNEL                          Kernel;
  EFI_PHYSICAL_ADDRESS            Address;
  UINT64                          ArgsAddress;
  UINT64                          TreeAddress;
  UINT64                          Used;
  UINT64                          WindowSize;
  UINT64                          RamdiskAddress;
  UINTN                           RamdiskOffset;
  UINTN                           RamdiskBytes;
  UINTN                           RamdiskExpandedSize;
  BOOLEAN                         RamdiskPacked;
  BOOLEAN                         KeepPacked;
  UINTN                           TrustCacheOffset;
  UINT64                          TrustCacheAddress;
  UINTN                           AddonOffset;
  UINT64                          AddonAddress;
  VOID                            *Policies;
  UINTN                           PolicySize;
  UINTN                           PolicyOffset;
  UINT64                          PolicyAddress;
  DARWIN_BOOT_ARGS                *Args;
  EFI_MEMORY_DESCRIPTOR           *Map;
  UINTN                           Capacity;
  UINTN                           MapSize;
  UINTN                           Key;
  UINTN                           Stride;
  UINT32                          Version;
  UINTN                           Attempt;
  CHAR8                           CommandLine[1024];
  UINT64                          Video[6];
  LAYOUT                          Layout;
  UINTN                           ExcludedOffset;
  UINTN                           ExcludedCapacity;
  UINTN                           ExcludedCount;
  UINT8                           *Excluded;
  UINT64                          HeadSize;
  UINT64                          Allocated;
  UINT64                          Cursor;
  UINTN                           InputsOffset;
  UINTN                           OutsideOffset;
  UINTN                           OutsideCapacity;
  UINTN                           OutsideCount;
  UINT64                          OutsideBytes;
  DARWIN_LOADER_INPUT             Inputs[DARWIN_INPUT_COUNT];
  UINT64                          Lead;
  UINT64                          KernelAddress;
  UINT64                          EntryAddress;
  EFI_PHYSICAL_ADDRESS            TrampolineBase;
  UINTN                           TrampolinePages;
  UINT64                          Trampoline;
  UINT64                          El2Vectors;

  (VOID)This;
  if ((Request == NULL) || (Request->Volume == NULL) || (Request->KernelPath == NULL)) { return EFI_INVALID_PARAMETER; }
  // At EL1 the kernel is entered as it is; at EL2 (the Pixel 8a) it is entered at EL1 below an EL2 that keeps
  // only DarwinEl2Trampoline (Entry.S).
  if ((Request->KextCount != 0) || (Request->DeviceTree != NULL) || (Request->DeviceTreeSize != 0) ||
      ((DarwinCurrentEl () != 4) && (DarwinCurrentEl () != 8))) {
    DarwinLog (L"DARWINBOOT: unsupported inputs or exception level; firmware retained\n");
    return EFI_UNSUPPORTED;
  }
  TrampolineBase = 0;
  TrampolinePages = 0;
  Trampoline = 0;
  El2Vectors = 0;
  if (!RamdiskLayout (Request->Ramdisk, Request->RamdiskSize, &RamdiskExpandedSize, &RamdiskPacked)) {
    DarwinLog (L"DARWINBOOT: ramdisk layout rejected, %lu bytes\n", (UINT64)Request->RamdiskSize);
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (CommandLine, sizeof (CommandLine));
  if (Request->BootArgs != NULL) {
    for (Index = 0; Request->BootArgs[Index] != 0; Index++) {
      if ((Index >= sizeof (CommandLine) - 1) || (Request->BootArgs[Index] > 0x7f)) {
        DarwinLog (L"DARWINBOOT: boot args rejected at %lu\n", (UINT64)Index);
        return EFI_BAD_BUFFER_SIZE;
      }
      CommandLine[Index] = (CHAR8)Request->BootArgs[Index];
    }
  }
  Root = NULL;
  Image = NULL;
  Tree = NULL;
  TrustCaches = NULL;
  Addon = NULL;
  AddonSize = 0;
  Policies = NULL;
  PolicySize = 0;
  TreePath = NULL;
  Map = NULL;
  Address = 0;
  Allocated = 0;
  HeadSize = 0;
  Excluded = NULL;
  ExcludedCount = 0;
  Capacity = 0;
  Status = gBS->HandleProtocol (Request->Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ReadFile (Root, Request->KernelPath, KERNEL_LIMIT, &Image, &ImageSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  Status = ParseKernel (Image, ImageSize, &Kernel);
  if (EFI_ERROR (Status)) { goto Fail; }
  Length = StrSize (Request->KernelPath);
  TreePath = AllocatePool (Length + 11 * sizeof (CHAR16));
  if (TreePath == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".adt");
  Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  Offset = 0;
  RamdiskOffset = MAX_UINTN;
  TrustCacheOffset = MAX_UINTN;
  AddonOffset = MAX_UINTN;
  PolicyOffset = MAX_UINTN;
  WindowSize = 0;
  KeepPacked = FALSE;
  ExcludedOffset = MAX_UINTN;
  ExcludedCapacity = 0;
  InputsOffset = MAX_UINTN;
  OutsideOffset = MAX_UINTN;
  OutsideCapacity = 0;
  if (!FindOutsideTable (Tree, TreeSize, &OutsideOffset, &OutsideCapacity)) { OutsideOffset = MAX_UINTN; }
  if (!AdtNode (Tree, TreeSize, &Offset, 0, 3, &RamdiskOffset, &TrustCacheOffset, &AddonOffset, &PolicyOffset, &WindowSize, &KeepPacked, &ExcludedOffset, &ExcludedCapacity, &InputsOffset) || (Offset != TreeSize)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (KeepPacked && (!RamdiskPacked || (RamdiskOffset == MAX_UINTN))) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (WindowSize == 0) { WindowSize = WINDOW_SIZE; }
  if (Kernel.Base > MAX_UINT64 - WindowSize) { Status = EFI_LOAD_ERROR; goto Fail; }
  if (Request->Ramdisk != NULL) {
    if ((RamdiskOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + RamdiskOffset) != 0) ||
        (U64 ((UINT8 *)Tree + RamdiskOffset + 8) != 0)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  } else if (RamdiskOffset != MAX_UINTN) {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".trustcache");
  Status = ReadFile (Root, TreePath, SIZE_16MB, &TrustCaches, &TrustCacheSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (TrustCaches != NULL) {
    if ((TrustCacheOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + TrustCacheOffset) != 0) ||
        (U64 ((UINT8 *)Tree + TrustCacheOffset + 8) != 0) || !ValidTrustCaches (TrustCaches, TrustCacheSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (TrustCacheOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  //
  // The GPU addon (kernel.macho.gpuaddon, scripts/make-gpu-addon.py): files the kernel writes into the Data volume before its addon
  // mounts. Only its header is checked here; the kernel validates every entry.
  //
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".gpuaddon");
  Status = ReadFile (Root, TreePath, SIZE_64MB, &Addon, &AddonSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (Addon != NULL) {
    if ((AddonOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + AddonOffset) != 0) || (U64 ((UINT8 *)Tree + AddonOffset + 8) != 0) ||
        (AddonSize < 24) || (CompareMem (Addon, "NGPUADD2", 8) != 0) || (U64 ((UINT8 *)Addon + 16) != AddonSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (AddonOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  // Root hash tables live outside the kernel's coalesced constant segments.
  // The kernel authenticates each envelope against its compiled owner anchor.
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".vroot");
  Status = ReadFile (Root, TreePath, SIZE_128MB, &Policies, &PolicySize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (Policies != NULL) {
    if ((PolicyOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + PolicyOffset) != 0) ||
        (U64 ((UINT8 *)Tree + PolicyOffset + 8) != 0) || (PolicySize < 32) ||
        (CompareMem (Policies, "VROOTB01", 8) != 0) || (U64 ((UINT8 *)Policies + 8) != 1) ||
        (U64 ((UINT8 *)Policies + 16) == 0) || (U64 ((UINT8 *)Policies + 16) > 16) ||
        (U64 ((UINT8 *)Policies + 24) != PolicySize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (PolicyOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  Status = ReadBootVideo (Video);
  if (EFI_ERROR (Status)) { goto Fail; }
  RamdiskBytes = ALIGN_VALUE (KeepPacked ? Request->RamdiskSize : RamdiskExpandedSize, EFI_PAGE_SIZE);
  // A window without excluded ranges starts on a block boundary and holds the kernel at its link address's offset
  // inside the first block (Lead): the boot arguments' bases are then block aligned, as XNU's first page tables
  // map whole blocks from the block that holds the kernel's header, all of it the kernel's own memory.
  Lead = 0;
  if (ExcludedOffset == MAX_UINTN) {
    Lead = Kernel.Base & (BLOCK_SIZE - 1);
    Status = AllocateWindow (&Kernel, &WindowSize, &Address);
    Allocated = WindowSize;
  } else {
    PlaceImage (BLOCK_SIZE + (Kernel.Base & (BLOCK_SIZE - 1)), &Kernel, TreeSize, RamdiskBytes, TrustCacheSize, AddonSize, PolicySize, &Layout);
    HeadSize = ALIGN_VALUE (Layout.Used + DARWIN_WINDOW_RESERVE (WindowSize), EFI_PAGE_SIZE);
    Status = AllocateSpan (&Kernel, WindowSize, HeadSize, &Address);
    Allocated = HeadSize;
  }
  if (EFI_ERROR (Status)) { goto Fail; }
  KernelAddress = Address + Lead;
  EntryAddress = KernelAddress + Kernel.Entry - Kernel.Base;
  PlaceImage (KernelAddress, &Kernel, TreeSize, RamdiskBytes, TrustCacheSize, AddonSize, PolicySize, &Layout);
  ArgsAddress = Layout.Args;
  TreeAddress = Layout.Tree;
  RamdiskAddress = Layout.Ramdisk;
  TrustCacheAddress = Layout.TrustCache;
  AddonAddress = Layout.Addon;
  PolicyAddress = Layout.Policy;
  Used = Layout.Used + Lead;
  if (Used > WindowSize - BLOCK_SIZE) {
    DarwinLog (L"DARWINBOOT: used %lu window %lu kernel %lu ramdisk %lu\n", (UINT64)Used, (UINT64)WindowSize, (UINT64)(Kernel.End - Kernel.Base), (UINT64)RamdiskBytes);
    Status = EFI_BAD_BUFFER_SIZE;
    goto Fail;
  }
  // At EL2 the kernel's cores get the EL2 code they keep: out of the image, into memory outside the window.
  if (DarwinCurrentEl () == 8) {
    TrampolinePages = EFI_SIZE_TO_PAGES (DarwinEl2TrampolineSize + SIZE_2KB);
    Status = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, TrampolinePages, &TrampolineBase);
    if (EFI_ERROR (Status)) { TrampolineBase = 0; goto Fail; }
    // (the vectors want 2 KiB alignment; the code is position independent)
    Trampoline = TrampolineBase + ((0 - (TrampolineBase + DarwinEl2VectorsOffset)) & (SIZE_2KB - 1));
    El2Vectors = Trampoline + DarwinEl2VectorsOffset;
    CopyMem ((VOID *)(UINTN)Trampoline, DarwinEl2Trampoline, (UINTN)DarwinEl2TrampolineSize);
    DarwinFlush (TrampolineBase, EFI_PAGES_TO_SIZE (TrampolinePages));
  }
  ZeroMem ((VOID *)(UINTN)Address, (UINTN)Used);
  for (Index = 0; Index < Kernel.Count; Index++) {
    CopyMem ((VOID *)(UINTN)(KernelAddress + Kernel.Segments[Index].Address - Kernel.Base),
             (UINT8 *)Image + Kernel.Segments[Index].FileOffset, (UINTN)Kernel.Segments[Index].FileSize);
  }
  Args = (DARWIN_BOOT_ARGS *)(UINTN)ArgsAddress;
  Args->Revision = 2;
  Args->Version = 2;
  Args->VirtualBase = Kernel.Base - Lead;
  Args->PhysicalBase = Address;
  Args->MemorySize = WindowSize;
  Args->MemorySizeActual = WindowSize;
  Args->TopOfKernelData = Address + Used;
  CopyMem (Args->Video, Video, sizeof (Video));
  Args->DeviceTree = Kernel.Base + TreeAddress - KernelAddress;
  Args->DeviceTreeLength = (UINT32)TreeSize;
  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));
  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);
  FillTree ((UINT8 *)(UINTN)TreeAddress, TreeSize, Address, WindowSize, Trampoline);
  if (Request->Ramdisk != NULL) {
    if (KeepPacked) { CopyMem ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize); }
    else { CopyRamdisk ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize, RamdiskPacked); }
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset), RamdiskAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset + 8), KeepPacked ? Request->RamdiskSize : RamdiskBytes);
  }
  if (TrustCaches != NULL) {
    CopyMem ((VOID *)(UINTN)TrustCacheAddress, TrustCaches, TrustCacheSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset), TrustCacheAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset + 8), TrustCacheSize);
    FreePool (TrustCaches); TrustCaches = NULL;
  }
  if (Addon != NULL) {
    CopyMem ((VOID *)(UINTN)AddonAddress, Addon, AddonSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + AddonOffset), AddonAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + AddonOffset + 8), AddonSize);
    FreePool (Addon); Addon = NULL;
  }
  if (Policies != NULL) {
    CopyMem ((VOID *)(UINTN)PolicyAddress, Policies, PolicySize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + PolicyOffset), PolicyAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + PolicyOffset + 8), PolicySize);
    FreePool (Policies); Policies = NULL;
  }
  FreePool (Image); Image = NULL;
  FreePool (Tree); Tree = NULL;
  FreePool (TreePath); TreePath = NULL;
  Root->Close (Root); Root = NULL;
  // Every file is read; the block device is asked once more, and only for a tree that wants the record.
  if (InputsOffset != MAX_UINTN) { LocateInputs (Request, Inputs); }
  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);
  if (EFI_ERROR (Status)) { goto Fail; }
  DarwinLog (L"DARWINBOOT: ARM64 va=0x%lx pa=0x%lx args=0x%lx entry=0x%lx owned=0x%lx\n",
         Kernel.Base, KernelAddress, ArgsAddress, EntryAddress, (UINT64)WindowSize);
  DarwinLog (L"DARWINBOOT: boot_args virtBase=0x%lx physBase=0x%lx memSize=0x%lx topOfKernelData=0x%lx tree=0x%lx+0x%x\n",
         Args->VirtualBase, Args->PhysicalBase, Args->MemorySize, Args->TopOfKernelData, Args->DeviceTree,
         Args->DeviceTreeLength);
  DarwinLog (L"DARWINBOOT: EL%lu MPIDR 0x%lx; EL2 trampoline 0x%lx vectors 0x%lx\n", DarwinCurrentEl () >> 2,
         DarwinMpidr (), Trampoline, El2Vectors);
  if (OutsideOffset != MAX_UINTN) {
    // As this map gives it; the table XNU reads comes from the map ExitBootServices accepts.
    OutsideCount = DarwinOutsideRanges (Map, MapSize, Stride, Address, WindowSize, SIZE_64KB, SIZE_1MB,
                                        (UINT8 *)(UINTN)(TreeAddress + OutsideOffset), OutsideCapacity, &OutsideBytes);
    DarwinLog (L"DARWINBOOT: free memory outside the window: %lu ranges of %lu, 0x%lx bytes\n",
           (UINT64)OutsideCount, (UINT64)OutsideCapacity, OutsideBytes);
    for (Index = 0; Index < OutsideCount; Index++) {
      DarwinLog (L"DARWINBOOT: outside 0x%lx + 0x%lx\n",
             DarwinRangeBase ((UINT8 *)(UINTN)(TreeAddress + OutsideOffset), Index),
             DarwinRangeSize ((UINT8 *)(UINTN)(TreeAddress + OutsideOffset), Index));
    }
  }
  if (InputsOffset != MAX_UINTN) {
    // The inputs as this map gives them; the records XNU reads come from the map ExitBootServices accepts.
    DarwinDescribeInputs (Map, &MapSize, Capacity, Stride, Address, WindowSize, ExcludedOffset != MAX_UINTN, Inputs);
    DarwinWriteInputs ((UINT8 *)(UINTN)(TreeAddress + InputsOffset), Inputs);
    DarwinLog (L"DARWINBOOT: payload disk 0x%lx + 0x%lx type %d state %x usable 0x%lx\n",
           Inputs[DARWIN_INPUT_PAYLOAD_DISK].Base, Inputs[DARWIN_INPUT_PAYLOAD_DISK].Size, (INT32)Inputs[DARWIN_INPUT_PAYLOAD_DISK].Type,
           Inputs[DARWIN_INPUT_PAYLOAD_DISK].State, Inputs[DARWIN_INPUT_PAYLOAD_DISK].Usable);
    DarwinLog (L"DARWINBOOT: initrd buffer 0x%lx + 0x%lx type %d state %x usable 0x%lx\n",
           Inputs[DARWIN_INPUT_INITRD].Base, Inputs[DARWIN_INPUT_INITRD].Size, (INT32)Inputs[DARWIN_INPUT_INITRD].Type,
           Inputs[DARWIN_INPUT_INITRD].State, Inputs[DARWIN_INPUT_INITRD].Usable);
  }
  if (ExcludedOffset != MAX_UINTN) {
    // The table as this map gives it; the map ExitBootServices accepts decides the one XNU reads.
    Excluded = (UINT8 *)(UINTN)(TreeAddress + ExcludedOffset);
    if (!DescribeWindow (Map, MapSize, Stride, Address, WindowSize, HeadSize, Excluded, ExcludedCapacity, Args, &ExcludedCount)) {
      Status = EFI_LOAD_ERROR;
      goto Fail;
    }
    DarwinLog (L"DARWINBOOT: window head=0x%lx usable=0x%lx excluded ranges=%lu of %lu\n",
           HeadSize, Args->MemorySizeActual, (UINT64)ExcludedCount, (UINT64)ExcludedCapacity);
    for (Index = 0; Index < ExcludedCount; Index++) {
      DarwinLog (L"DARWINBOOT: excluded 0x%lx + 0x%lx\n", DarwinRangeBase (Excluded, Index), DarwinRangeSize (Excluded, Index));
    }
  }
  DarwinLog (L"DARWINBOOT: video base=0x%lx display=%lu row=%lu width=%lu height=%lu depth=%lu\n",
         Video[0], Video[1], Video[2], Video[3], Video[4], Video[5]);
  DarwinLog (L"DARWINBOOT: command line: %a\n", CommandLine);
  DarwinLog (L"DARWINBOOT: entry words %08x %08x %08x %08x\n", U32 ((VOID *)(UINTN)EntryAddress),
             U32 ((VOID *)(UINTN)(EntryAddress + 4)), U32 ((VOID *)(UINTN)(EntryAddress + 8)), U32 ((VOID *)(UINTN)(EntryAddress + 12)));
  DarwinLog (L"DARWINBOOT: inputs validated; exiting boot services\n");

  //
  // The same facts in large type over the whole screen. The firmware's log
  // draws far larger than the text console, and the form feed clears the
  // screen first. This is what stays visible if the kernel never draws.
  //
  DEBUG ((DEBUG_ERROR, "\fDARWINBOOT: hand-off to the kernel\n\n"));
  DEBUG ((DEBUG_ERROR, "kernel   va 0x%lx  pa 0x%lx\n", Kernel.Base, Address));
  DEBUG ((DEBUG_ERROR, "entry    pa 0x%lx\n", EntryAddress));
  DEBUG ((DEBUG_ERROR, "args     pa 0x%lx  window 0x%lx bytes\n", ArgsAddress, (UINT64)WindowSize));
  if (Excluded != NULL) {
    DEBUG ((DEBUG_ERROR, "memory   usable 0x%lx  excluded ranges %lu\n", Args->MemorySizeActual, (UINT64)ExcludedCount));
  }
  if (InputsOffset != MAX_UINTN) {
    DEBUG ((DEBUG_ERROR, "disk     0x%lx + 0x%lx  type %d  state %x\n", Inputs[DARWIN_INPUT_PAYLOAD_DISK].Base,
            Inputs[DARWIN_INPUT_PAYLOAD_DISK].Size, (INT32)Inputs[DARWIN_INPUT_PAYLOAD_DISK].Type, Inputs[DARWIN_INPUT_PAYLOAD_DISK].State));
    DEBUG ((DEBUG_ERROR, "initrd   0x%lx + 0x%lx  type %d  state %x\n", Inputs[DARWIN_INPUT_INITRD].Base,
            Inputs[DARWIN_INPUT_INITRD].Size, (INT32)Inputs[DARWIN_INPUT_INITRD].Type, Inputs[DARWIN_INPUT_INITRD].State));
  }
  DEBUG ((DEBUG_ERROR, "video    base 0x%lx  %lux%lu  row %lu bytes  %lu bpp\n", Video[0], Video[3], Video[4], Video[2], Video[5]));
  DEBUG ((DEBUG_ERROR, "entry words %08x %08x %08x %08x\n",
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(EntryAddress)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(EntryAddress + 4)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(EntryAddress + 8)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(EntryAddress + 12))));
  DEBUG ((DEBUG_ERROR, "CurrentEL %lu  SCTLR_EL1 0x%lx  TCR_EL1 0x%lx\n", DarwinCurrentEl () >> 2, READ_SYSREG (sctlr_el1), READ_SYSREG (tcr_el1)));
  DEBUG ((DEBUG_ERROR, "ID_AA64MMFR0_EL1 0x%lx  (PARange %lu, 4 KiB granule %a)\n", READ_SYSREG (id_aa64mmfr0_el1),
          READ_SYSREG (id_aa64mmfr0_el1) & 0xF, (((READ_SYSREG (id_aa64mmfr0_el1) >> 28) & 0xF) == 0xF) ? "NOT supported" : "supported"));
  DEBUG ((DEBUG_ERROR, "ID_AA64PFR0_EL1  0x%lx\n", READ_SYSREG (id_aa64pfr0_el1)));
  DEBUG ((DEBUG_ERROR, "\nExiting boot services. A green bar across the top\nmarks the loader handoff before the kernel branch.\n"));
  for (Attempt = 0; Attempt < 3; Attempt++) {
    MapSize = Capacity;
    Status = gBS->GetMemoryMap (&MapSize, Map, &Key, &Stride, &Version);
    if (EFI_ERROR (Status)) {
      if (Attempt == 0) { goto Fail; }
      CpuDeadLoop ();
    }
    // No boot service runs between this map and ExitBootServices, so the records and the tables describe the
    // memory XNU gets and the free memory outside it.
    if (OutsideOffset != MAX_UINTN) {
      OutsideCount = DarwinOutsideRanges (Map, MapSize, Stride, Address, WindowSize, SIZE_64KB, SIZE_1MB,
                                          (UINT8 *)(UINTN)(TreeAddress + OutsideOffset), OutsideCapacity, &OutsideBytes);
    }
    if (InputsOffset != MAX_UINTN) {
      DarwinDescribeInputs (Map, &MapSize, Capacity, Stride, Address, WindowSize, Excluded != NULL, Inputs);
      DarwinWriteInputs ((UINT8 *)(UINTN)(TreeAddress + InputsOffset), Inputs);
    }
    if ((Excluded != NULL) &&
        !DescribeWindow (Map, MapSize, Stride, Address, WindowSize, HeadSize, Excluded, ExcludedCapacity, Args, &ExcludedCount)) {
      if (Attempt == 0) { Status = EFI_LOAD_ERROR; goto Fail; }
      CpuDeadLoop ();
    }
    Status = gBS->ExitBootServices (mImage, Key);
    if (!EFI_ERROR (Status)) {
      if (Excluded == NULL) {
        DarwinEnter (ArgsAddress, EntryAddress, Address, WindowSize, Video[0], Video[2] * Video[4], El2Vectors);
        CpuDeadLoop ();
      }
      // The usable memory behind the head leaves the data cache as the head does in DarwinEnter. An excluded
      // range is not touched: the firmware may not even map it.
      Cursor = Address + HeadSize;
      for (Index = 0; Index < ExcludedCount; Index++) {
        if (DarwinRangeBase (Excluded, Index) > Cursor) { DarwinFlush (Cursor, DarwinRangeBase (Excluded, Index) - Cursor); }
        Cursor = DarwinRangeBase (Excluded, Index) + DarwinRangeSize (Excluded, Index);
      }
      if (Address + WindowSize > Cursor) { DarwinFlush (Cursor, Address + WindowSize - Cursor); }
      DarwinEnter (ArgsAddress, EntryAddress, Address, HeadSize, Video[0], Video[2] * Video[4], El2Vectors);
      CpuDeadLoop ();
    }
    if (Status != EFI_INVALID_PARAMETER) { CpuDeadLoop (); }
  }
  CpuDeadLoop ();
  return EFI_ABORTED;
Fail:
  if (Root != NULL) { Root->Close (Root); }
  if (Image != NULL) { FreePool (Image); }
  if (Tree != NULL) { FreePool (Tree); }
  if (TrustCaches != NULL) { FreePool (TrustCaches); }
  if (Addon != NULL) { FreePool (Addon); }
  if (Policies != NULL) { FreePool (Policies); }
  if (TreePath != NULL) { FreePool (TreePath); }
  if (Map != NULL) { FreePool (Map); }
  if (Address != 0) { gBS->FreePages (Address, EFI_SIZE_TO_PAGES (Allocated)); }
  if (TrampolineBase != 0) { gBS->FreePages (TrampolineBase, TrampolinePages); }
  DarwinLog (L"DARWINBOOT: boot refused: %r; firmware retained\n", Status);
  return Status;
}

STATIC UNIXNU_LOADER_PROTOCOL mLoader = { UNIXNU_LOADER_REVISION, Boot };

STATIC
EFI_STATUS
EFIAPI
Unload (EFI_HANDLE ImageHandle)
{
  return gBS->UninstallMultipleProtocolInterfaces (ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
}

EFI_STATUS
EFIAPI
DarwinBootEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_STATUS               Status;
  VOID                     *Existing;

  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, &Existing);
  if (!EFI_ERROR (Status)) { return EFI_ALREADY_STARTED; }
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  mImage = ImageHandle;
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
  if (!EFI_ERROR (Status)) {
    Loaded->Unload = Unload;
    DarwinLog (L"DARWINBOOT: loader v2 installed EL%u\n", (UINT32)(DarwinCurrentEl () >> 2));
  }
  return Status;
}

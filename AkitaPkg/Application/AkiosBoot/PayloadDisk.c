/** @file
  AkiosBoot: the ESP image in the boot payload as a read-only block device, for the FAT driver to mount -- the
  same files a UFS ESP will hold later, so UniXNU sees one layout either way.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "AkiosBoot.h"

#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>

#define SECTOR  512

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} DISK_PATH;
#pragma pack()

STATIC DISK_PATH  mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0 }   // gAkitaPayloadDiskGuid, set at publication
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC CONST UINT8          *mData;
STATIC EFI_BLOCK_IO_MEDIA   mMedia;

STATIC
EFI_STATUS
EFIAPI
DiskReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                Extended
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DiskRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  if ((BufferSize % SECTOR) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / SECTOR) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mData + Lba * SECTOR, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DiskWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
DiskFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  DiskReset,
  DiskRead,
  DiskWrite,
  DiskFlush
};

//
// Disk I/O on the same memory: EDK2's FAT driver mounts on it, and a firmware without EDK2's DiskIoDxe (U-Boot)
// does not layer it over BlockIo by itself.
//
STATIC
EFI_STATUS
EFIAPI
DiskIoRead (
  IN  EFI_DISK_IO_PROTOCOL  *This,
  IN  UINT32                MediaId,
  IN  UINT64                Offset,
  IN  UINTN                 BufferSize,
  OUT VOID                  *Buffer
  )
{
  UINT64  Size;

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  Size = (mMedia.LastBlock + 1) * SECTOR;
  if ((Offset > Size) || (BufferSize > Size - Offset) || ((Buffer == NULL) && (BufferSize != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mData + Offset, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DiskIoWrite (
  IN EFI_DISK_IO_PROTOCOL  *This,
  IN UINT32                MediaId,
  IN UINT64                Offset,
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC EFI_DISK_IO_PROTOCOL  mDiskIo = {
  EFI_DISK_IO_PROTOCOL_REVISION,
  DiskIoRead,
  DiskIoWrite
};

EFI_STATUS
AkiosPublishDisk (
  IN CONST VOID  *Data,
  IN UINTN       Size
  )
{
  EFI_HANDLE  Handle;

  if ((Data == NULL) || (Size < SECTOR) || ((Size % SECTOR) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  mData = Data;
  CopyGuid (&mPath.Vendor.Guid, &gAkitaPayloadDiskGuid);
  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId          = 1;
  mMedia.MediaPresent     = TRUE;
  mMedia.ReadOnly         = TRUE;
  mMedia.BlockSize        = SECTOR;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = Size / SECTOR - 1;

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiDevicePathProtocolGuid,
                &mPath,
                &gEfiBlockIoProtocolGuid,
                &mBlockIo,
                &gEfiDiskIoProtocolGuid,
                &mDiskIo,
                NULL
                );
}

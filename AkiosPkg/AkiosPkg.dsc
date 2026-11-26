## @file
#  AkIOS's loader, the same for every device: UniXNU.efi (the boot manager our firmware starts from the ESP),
#  DarwinBoot.efi (the XNU loader driver) and Fat.efi (EDK2's FAT driver).
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME           = Akios
  PLATFORM_GUID           = 0E7A5C93-2B1F-4D86-A4C7-58F1E3B90D26
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x0001001C
  OUTPUT_DIRECTORY        = Build/Akios
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS           = DEBUG|RELEASE
  SKUID_IDENTIFIER        = DEFAULT

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
!if $(TARGET) == RELEASE
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
!else
  DebugLib|MdePkg/Library/UefiDebugLibStdErr/UefiDebugLibStdErr.inf
!endif

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000000
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x07
  # the config on the ESP: \EFI\UniXNU\akios.toml (UniXNULib keeps its paths under \EFI\UniXNU)
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUConfigPath|L"\\EFI\\UniXNU\\akios.toml"

[Components]
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
  FatPkg/EnhancedFatDxe/Fat.inf

[Defines]
  PLATFORM_NAME           = corexaen
  PLATFORM_GUID           = 32ac5d70-874c-4221-9b23-123a782f84e8
  TOOLCHAIN_TAG           = VS2022
  TARGET_ARCH             = X64
  PLATFORM_VERSION        = 1.0
  BUILD_TARGET            = UEFI_APPLICATION
  BUILD_TARGETS           = RELEASE
  SUPPORTED_ARCHITECTURES = X64
  PLATFORM_TYPE           = UEFI
  DSC_SPECIFICATION              = 0x00010005

[LibraryClasses]
  UefiLib | C:\edk2\MdePkg\Library\UefiLib\UefiLib.inf
  UefiApplicationEntryPoint | C:\edk2\MdePkg\Library\UefiApplicationEntryPoint\UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib | C:\edk2\MdePkg\Library\UefiBootServicesTableLib\UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib | C:\edk2\MdePkg\Library\UefiRuntimeServicesTableLib\UefiRuntimeServicesTableLib.inf
  UefiStackCheckLib | C:\edk2\MdePkg\Library\StackCheckLibNull\StackCheckLibNull.inf
  BaseLib | C:\edk2\MdePkg\Library\BaseLib\BaseLib.inf
  BaseMemoryLib | C:\edk2\MdePkg\Library\BaseMemoryLib\BaseMemoryLib.inf
  MemoryAllocationLib | C:\edk2\MdePkg\Library\UefiMemoryAllocationLib\UefiMemoryAllocationLib.inf
  DevicePathLib | C:\edk2\MdePkg\Library\UefiDevicePathLib\UefiDevicePathLib.inf
  PrintLib | C:\edk2\MdePkg\Library\BasePrintLib\BasePrintLib.inf
  PcdLib | C:\edk2\MdePkg\Library\BasePcdLibNull\BasePcdLibNull.inf
  DebugLib | C:\edk2\MdePkg\Library\BaseDebugLibNull\BaseDebugLibNull.inf
  RegisterFilterLib | C:\edk2\MdePkg\Library\RegisterFilterLibNull\RegisterFilterLibNull.inf

[Packages]
  C:\edk2\MdePkg\MdePkg.dec
  C:\edk2\MdeModulePkg\MdeModulePkg.dec

[Components]
  C:\Users\nanno\Desktop\uefi\os.inf

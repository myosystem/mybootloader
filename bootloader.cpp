#define _CTR_SCEURE_NO_WARNINGS
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS
EFIAPI
efi_main (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
    Print((CHAR16*)L"Hello, World!\n");
    return EFI_SUCCESS;
}

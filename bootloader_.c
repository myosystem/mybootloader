#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>  // Add this line to include EFI_LOADED_IMAGE
#include <Guid/FileInfo.h>

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_HANDLE *HandleBuffer;
    UINTN HandleCount;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    EFI_FILE_PROTOCOL *Root;
    EFI_FILE_PROTOCOL *File;

    VOID *Buffer = NULL;
    EFI_HANDLE LoadedImageHandle = NULL;

    Print(L"UEFI Bootloader: Loading and executing file...\n");

    // Locate all handles that support Simple File System Protocol
    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandleCount, &HandleBuffer);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to locate handles with Simple File System Protocol: %r\n", Status);
        gBS->Stall(5000000); // Stall for 5 seconds before returning
        return Status;
    }

    // Retrieve the Simple File System Protocol from the first handle
    Status = gBS->HandleProtocol(HandleBuffer[0], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to get Simple File System Protocol from handle: %r\n", Status);
        FreePool(HandleBuffer);
        gBS->Stall(5000000); // Stall for 5 seconds before returning
        return Status;
    }

    // Free the handle buffer as it's no longer needed
    FreePool(HandleBuffer);

    // Open the root directory of the file system
    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to open root directory: %r\n", Status);
        gBS->Stall(5000000); // Stall for 5 seconds before returning
        return Status;
    }

    // Open the file to be executed (e.g., os.efi)
    Status = Root->Open(Root, &File, L"EFI\\Boot\\os.efi", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to open file: %r\n", Status);
        gBS->Stall(5000000); // Stall for 5 seconds before returning
        return Status;
    }

    // Get the file size
    EFI_FILE_INFO *FileInfo;
    UINTN FileInfoSize = 0;

    // Get the size of the file info structure
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &FileInfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"Failed to get file info size: %r\n", Status);
        File->Close(File);
        gBS->Stall(5000000);
        return Status;
    }

    // Allocate memory for file info structure
    FileInfo = AllocateZeroPool(FileInfoSize);
    if (FileInfo == NULL) {
        Print(L"Failed to allocate memory for file info.\n");
        File->Close(File);
        gBS->Stall(5000000);
        return EFI_OUT_OF_RESOURCES;
    }

    // Retrieve the file info
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to retrieve file info: %r\n", Status);
        FreePool(FileInfo);
        File->Close(File);
        gBS->Stall(5000000);
        return Status;
    }

    Print(L"File Size: %llu bytes\n", FileInfo->FileSize);
    UINTN FileSize = FileInfo->FileSize;
    FreePool(FileInfo);

    // Load the image (os.efi) into memory
    Buffer = AllocateZeroPool(FileSize);
    if (Buffer == NULL) {
        Print(L"Failed to allocate memory for file buffer.\n");
        gBS->Stall(5000000); // Stall for 5 seconds
        File->Close(File);
        return EFI_OUT_OF_RESOURCES;
    }

    // Read the file into memory
    Status = File->Read(File, &FileSize, Buffer);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to read file: %r\n", Status);
        gBS->Stall(5000000); // Stall for 5 seconds
        FreePool(Buffer);
        File->Close(File);
        return Status;
    }

    File->Close(File);
    
    // Load the image from memory
    Status = gBS->LoadImage(FALSE, ImageHandle, NULL, Buffer, FileSize, &LoadedImageHandle);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to load image: %r\n", Status);
        gBS->Stall(5000000); // Stall for 5 seconds
        FreePool(Buffer);
        return Status;
    }

    // Start the loaded image
    Status = gBS->StartImage(LoadedImageHandle, NULL, NULL);
    if (EFI_ERROR(Status)) {
        Print(L"Failed to start image: %r\n", Status);
        gBS->Stall(5000000); // Stall for 5 seconds
    }

    FreePool(Buffer);
    return EFI_SUCCESS;
}

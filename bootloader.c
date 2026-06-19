#include <Uefi.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CpuLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Library/DevicePathLib.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>

#include <Register/Intel/ArchitecturalMsr.h>

#include <IndustryStandard/Pci.h>

extern void jump_to_address(void* stack_top,void* addr);
extern void load_gdt(void* pml4_phys);
extern void hlt_loop(void);
extern void stint();
typedef unsigned long long u64;
typedef unsigned long long uptr;

#define P    (1ull<<0)   // Present
#define RW   (1ull<<1)   // Read/Write
#define US   (1ull<<2)   // User/Supervisor (0 = supervisor)
#define PWT  (1ull<<3)   // Write-Through
#define PCD  (1ull<<4)   // Cache Disable
#define ABIT (1ull<<5)   // Accessed
#define PS   (1ull<<7)   // Page Size (2MiB/1GiB)
#define G    (1ull<<8)   // Global
#define NX   (1ull<<63)  // No-Execute (none use)

#define PAGE_4K  (4096ull)
#define PAGE_2M  (2ull * 1024 * 1024)
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ull  // ?? 12?? ??? ???
#define PD_2M_MASK   0x000FFFFFFFE00000ull  // 2MiB ???? ???? ???
#define KERNEL_BASE_VA  0xFFFFFFFF80000000ull
#define HHDM_BASE       0xFFFFFF0000000000ULL
#define HHDM_PML4_INDEX 510
#define MMIO_BASE 0xFFFFFE8000000000ULL
static inline u64 AlignDown(u64 x, u64 a) { return x & ~(a - 1); }
static inline u64 AlignUp  (u64 x, u64 a) { return (x + a - 1) & ~(a - 1); }
static inline u64 AlignUp2M(u64 x){ return (x + (PAGE_2M-1)) & ~(PAGE_2M-1); }
typedef struct {
    unsigned long long* buf;
    u64 bits;
} PhysBitmap;
static inline void pb_set(PhysBitmap* bm, UINT64 page_idx) {
    bm->buf[page_idx >> 6] |=  (UINT64)(1ull << (page_idx & 63));
}
static inline void pb_clear(PhysBitmap* bm, UINT64 page_idx) {
    bm->buf[page_idx >> 6] &= (UINT64)~(1ull << (page_idx & 63));
}
static inline BOOLEAN pb_test(const PhysBitmap* bm, UINT64 page_idx) {
    return (BOOLEAN)((bm->buf[page_idx >> 6] >> (page_idx & 63)) & 1ull);
}
PhysBitmap phys_bitmap;
static void pb_mark_used_range(PhysBitmap* bm, UINT64 phys_start, UINT64 bytes) {
    UINT64 first = phys_start / PAGE_4K;
    UINT64 last  = (phys_start + bytes + PAGE_4K - 1) / PAGE_4K; // exclusive
    if (last > bm->bits) last = bm->bits;
    for (UINT64 i = first; i < last; ++i) pb_set(bm, i);
}
static void pb_mark_free_range(PhysBitmap* bm, UINT64 phys_start, UINT64 bytes) {
    UINT64 first = phys_start / PAGE_4K;
    UINT64 last  = (phys_start + bytes + PAGE_4K - 1) / PAGE_4K; // exclusive
    if (last > bm->bits) last = bm->bits;
    for (UINT64 i = first; i < last; ++i) pb_clear(bm, i);
}
// UEFI?? 4KiB ?? ??? ?? ???? 0?? ???
static EFI_STATUS AllocPhysPage(EFI_PHYSICAL_ADDRESS* OutPhys) {
    EFI_STATUS st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, OutPhys);
    if (EFI_ERROR(st)) return st;
    pb_mark_used_range(&phys_bitmap, *OutPhys, PAGE_4K);
    // ???? ???? ??? ?????? ??=?? ????? ?? ??
    SetMem((VOID*)(uptr)(*OutPhys), PAGE_4K, 0);
    return EFI_SUCCESS;
}

static inline volatile u64* PhysToPtr(EFI_PHYSICAL_ADDRESS phys) {
    return (volatile u64*)(uptr)phys; // ????? ??
}
static inline volatile u64* Tbl(u64 phys){ return PhysToPtr(phys & PT_ADDR_MASK); }

// ??? ?? (IA-32e 4??)
static inline u64 IdxPml4(u64 va) { return (va >> 39) & 0x1FF; }
static inline u64 IdxPdpt(u64 va) { return (va >> 30) & 0x1FF; }
static inline u64 IdxPd  (u64 va) { return (va >> 21) & 0x1FF; }
static inline u64 IdxPt  (u64 va) { return (va >> 12) & 0x1FF; }

// PML4 -> PDPT ??
static EFI_STATUS EnsurePdpt(EFI_PHYSICAL_ADDRESS pml4_phys, u64 va, EFI_PHYSICAL_ADDRESS* out_pdpt_phys) {
    volatile u64* pml4 = PhysToPtr(pml4_phys);
    u64 i = IdxPml4(va);
    if ((pml4[i] & P) == 0) {
        EFI_PHYSICAL_ADDRESS newp;
        EFI_STATUS st = AllocPhysPage(&newp); if (EFI_ERROR(st)) return st;
        pml4[i] = (newp & PT_ADDR_MASK) | P | RW; // supervisor
    }
    *out_pdpt_phys = (pml4[i] & PT_ADDR_MASK);
    return EFI_SUCCESS;
}

// PDPT -> PD ??
static EFI_STATUS EnsurePd(EFI_PHYSICAL_ADDRESS pml4_phys, u64 va, EFI_PHYSICAL_ADDRESS* out_pd_phys) {
    EFI_STATUS st;
    EFI_PHYSICAL_ADDRESS pdpt_phys;
    st = EnsurePdpt(pml4_phys, va, &pdpt_phys); if (EFI_ERROR(st)) return st;
    volatile u64* pdpt = PhysToPtr(pdpt_phys);
    u64 i = IdxPdpt(va);
    if ((pdpt[i] & P) == 0) {
        EFI_PHYSICAL_ADDRESS newp;
        st = AllocPhysPage(&newp); if (EFI_ERROR(st)) return st;
        pdpt[i] = (newp & PT_ADDR_MASK) | P | RW;
    }
    *out_pd_phys = (pdpt[i] & PT_ADDR_MASK);
    return EFI_SUCCESS;
}

// PD -> PT ?? (4KiB?). ?? 2MiB ????? ??.
static EFI_STATUS EnsurePt(EFI_PHYSICAL_ADDRESS pml4_phys, u64 va, EFI_PHYSICAL_ADDRESS* out_pt_phys) {
    EFI_STATUS st;
    EFI_PHYSICAL_ADDRESS pd_phys;
    st = EnsurePd(pml4_phys, va, &pd_phys); if (EFI_ERROR(st)) return st;
    volatile u64* pd = PhysToPtr(pd_phys);
    u64 i = IdxPd(va);
    if ((pd[i] & P) == 0) {
        EFI_PHYSICAL_ADDRESS newp;
        st = AllocPhysPage(&newp); if (EFI_ERROR(st)) return st;
        pd[i] = (newp & PT_ADDR_MASK) | P | RW; // ?? PT? ???
    } else if (pd[i] & PS) {
        return EFI_DEVICE_ERROR; // ?? 2MiB ?? ??
    }
    *out_pt_phys = (pd[i] & PT_ADDR_MASK);
    return EFI_SUCCESS;
}

// 2MiB ??
static EFI_STATUS Map2M(EFI_PHYSICAL_ADDRESS pml4_phys, u64 va, u64 pa, u64 flags) {
    if ((va & (PAGE_2M - 1)) || (pa & (PAGE_2M - 1))) return EFI_INVALID_PARAMETER;
    EFI_STATUS st;
    EFI_PHYSICAL_ADDRESS pd_phys;
    st = EnsurePd(pml4_phys, va, &pd_phys); if (EFI_ERROR(st)) return st;
    volatile u64* pd = PhysToPtr(pd_phys);
    u64 i = IdxPd(va);
    pd[i] = (pa & PD_2M_MASK) | P | PS | (flags & ~PS);
    return EFI_SUCCESS;
}

// 4KiB ??
static EFI_STATUS Map4K(EFI_PHYSICAL_ADDRESS pml4_phys, u64 va, u64 pa, u64 flags) {
    if ((va & (PAGE_4K - 1)) || (pa & (PAGE_4K - 1))) return EFI_INVALID_PARAMETER;
    EFI_STATUS st;
    EFI_PHYSICAL_ADDRESS pt_phys;
    st = EnsurePt(pml4_phys, va, &pt_phys); if (EFI_ERROR(st)) return st;
    volatile u64* pt = PhysToPtr(pt_phys);
    u64 i = IdxPt(va);
    pt[i] = (pa & PT_ADDR_MASK) | P | (flags & ~PT_ADDR_MASK);
    return EFI_SUCCESS;
}
static EFI_STATUS EnsureHhdmPdpt(EFI_PHYSICAL_ADDRESS pml4_phys, EFI_PHYSICAL_ADDRESS* out_pdpt_phys){
    volatile u64* pml4 = PhysToPtr(pml4_phys);
    if ((pml4[HHDM_PML4_INDEX] & P) == 0){
        EFI_PHYSICAL_ADDRESS newp;
        EFI_STATUS st = AllocPhysPage(&newp); if (EFI_ERROR(st)) return st;
        pml4[HHDM_PML4_INDEX] = (newp & PT_ADDR_MASK) | P | RW; // U=0
    }
    *out_pdpt_phys = (pml4[HHDM_PML4_INDEX] & PT_ADDR_MASK);
    return EFI_SUCCESS;
}
// ??? ?????? ?? (?/? ????? 4KiB, ???? 2MiB)
static EFI_STATUS MapIdentityRange(EFI_PHYSICAL_ADDRESS pml4_phys, u64 phys_start, u64 bytes, u64 flags) {
    u64 cur = phys_start;
    u64 end = phys_start + bytes;

    while (cur < end && (cur & (PAGE_2M - 1))) {
        EFI_STATUS st = Map4K(pml4_phys, cur, cur, flags); if (EFI_ERROR(st)) return st;
        cur += PAGE_4K;
    }
    while ((end - cur) >= PAGE_2M) {
        EFI_STATUS st = Map2M(pml4_phys, cur, cur, flags); if (EFI_ERROR(st)) return st;
        cur += PAGE_2M;
    }
    while (cur < end) {
        EFI_STATUS st = Map4K(pml4_phys, cur, cur, flags); if (EFI_ERROR(st)) return st;
        cur += PAGE_4K;
    }
    return EFI_SUCCESS;
}
EFI_STATUS MapRangeVaToPa(EFI_PHYSICAL_ADDRESS pml4_phys,
                          u64 va, u64 pa, u64 bytes, u64 flags)
{
    pb_mark_used_range(&phys_bitmap, pa, bytes);
    EFI_STATUS st;
    u64 cur_va = va;
    u64 cur_pa = pa;
    u64 end_va = va + bytes; // bytes? ? ? overflow ????? ?? ?? ??

    // 1) ? ????: 2MiB ??? ?? ?? ??? 4KiB?
    //    (? ? 2MiB ??? ??? 2MiB ???? ??)
    while (cur_va < end_va) {
        // ??? ?? ??? 2MiB ???? ????
        if ( ((cur_va | cur_pa) % PAGE_2M == 0) && ((end_va - cur_va) >= PAGE_2M) )
            break;

        // 2MiB?? ?? ?? ? ?? ?, ?? ?? ??? 4KiB ??? ???
        u64 next2m_va = AlignUp(cur_va, PAGE_2M);
        u64 next2m_pa = AlignUp(cur_pa, PAGE_2M);
        u64 edge      = next2m_va - cur_va;
        u64 edge_pa   = next2m_pa - cur_pa;
        if (edge_pa < edge) edge = edge_pa;
        if (edge == 0) edge = PAGE_4K;              // ?? ? ???
        if (edge > (end_va - cur_va)) edge = end_va - cur_va;

        // 4KiB? edge ?? ??
        u64 to_map = edge & ~(PAGE_4K - 1);         // 4KiB ??? ??
        if (to_map == 0) to_map = PAGE_4K;
        for (u64 off = 0; off < to_map; off += PAGE_4K) {
            st = Map4K(pml4_phys, cur_va + off, cur_pa + off, flags);
            if (EFI_ERROR(st)) return st;
        }
        cur_va += to_map;
        cur_pa += to_map;
    }

    // 2) ???: 2MiB ???? ??
    while ((end_va - cur_va) >= PAGE_2M) {
        // ? ???? cur_va/cur_pa? ? ? 2MiB ??? ??? ??
        if ( (cur_va % PAGE_2M) || (cur_pa % PAGE_2M) ) break;
        st = Map2M(pml4_phys, cur_va, cur_pa, flags);
        if (EFI_ERROR(st)) return st;
        cur_va += PAGE_2M;
        cur_pa += PAGE_2M;
    }

    // 3) ? ????: ?? ? 4KiB? ???
    while (cur_va < end_va) {
        st = Map4K(pml4_phys, cur_va, cur_pa, flags);
        if (EFI_ERROR(st)) return st;
        cur_va += PAGE_4K;
        cur_pa += PAGE_4K;
    }

    return EFI_SUCCESS;
}
// UEFI ??? ?? -> ??? ??? (???)
// MMIO/UC? ???(PCD|PWT), ? ?? ?? ??.
static u64 FlagsFromDesc(const EFI_MEMORY_DESCRIPTOR* d) {
    u64 f = RW | G; // supervisor, global
    BOOLEAN is_mmio = (d->Type == EfiMemoryMappedIO) || (d->Type == EfiMemoryMappedIOPortSpace);
    BOOLEAN is_uc   = ( (d->Attribute & EFI_MEMORY_UC) != 0 );
    if (is_mmio || is_uc) f |= (PCD | PWT);
    return f;
}
static BOOLEAN IsMmioType(UINT32 t){
    return t==EfiMemoryMappedIO || t==EfiMemoryMappedIOPortSpace;
}
static BOOLEAN IsRamType(UINT32 t){
    return t==EfiConventionalMemory || t==EfiLoaderCode || t==EfiLoaderData ||
           t==EfiBootServicesCode   || t==EfiBootServicesData ||
           t==EfiACPIReclaimMemory; // ??
}
static u64 FlagsForRam(void){ return RW | G | NX; }
static u64 FlagsForMmio(void){ return RW | G | PCD | PWT | NX; }
static EFI_STATUS MapIdentityRange4K(EFI_PHYSICAL_ADDRESS pml4_phys, u64 phys, u64 bytes, u64 flags){
    u64 cur = AlignDown(phys, PAGE_4K), end = AlignUp(phys+bytes, PAGE_4K);
    while(cur < end){
        EFI_STATUS st = Map4K(pml4_phys, cur, cur, P | flags);
        if(EFI_ERROR(st)) return st;
        cur += PAGE_4K;
    }
    return EFI_SUCCESS;
}
EFI_PHYSICAL_ADDRESS refcount_phys;
// ?? API: ????? ?? UEFI? ??? ?? ??? ????? ??
EFI_STATUS BuildIdentityPageTablesFromUefi(EFI_PHYSICAL_ADDRESS* out_pml4_phys) {
    EFI_STATUS st;

    // 1) ? PML4
    EFI_PHYSICAL_ADDRESS pml4_phys = 0;
    st = AllocPhysPage(&pml4_phys); if (EFI_ERROR(st)) return st;

    // 2) ???? ????
    UINTN mmSize = 0, mapKey = 0, descSize = 0; UINT32 descVer = 0;
    st = gBS->GetMemoryMap(&mmSize, NULL, &mapKey, &descSize, &descVer);
    if (st != EFI_BUFFER_TOO_SMALL) return st;

    mmSize += 2 * descSize; // ??
    EFI_MEMORY_DESCRIPTOR* mm = NULL;
    st = gBS->AllocatePool(EfiLoaderData, mmSize, (VOID**)&mm);
    if (EFI_ERROR(st)) return st;

    st = gBS->GetMemoryMap(&mmSize, mm, &mapKey, &descSize, &descVer);
    if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }

    u64 top = 0;
    for(u64 off = 0; off < mmSize; off += descSize){
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)mm + off);
        UINT64 start = d->PhysicalStart;
        if (d->Type == EfiConventionalMemory ||
            d->Type == EfiBootServicesCode ||
            d->Type == EfiBootServicesData ||
            d->Type == EfiLoaderCode ||
            d->Type == EfiLoaderData) {
            UINT64 end = start + ((UINT64)d->NumberOfPages << 12);
            if (end > top) top = end;
        }
    }
    u64 total_pages = (top + PAGE_4K - 1) / PAGE_4K;
    u64 bitmap_size = (total_pages + 63) / 64 * 8;
    u64 bitmap_pages = (bitmap_size + PAGE_4K - 1) / PAGE_4K;
	u64 refcount_size = total_pages * sizeof(UINT64);
	u64 refcount_pages = (refcount_size + PAGE_4K - 1) / PAGE_4K;
    EFI_PHYSICAL_ADDRESS bitmap_phys;
    st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, bitmap_pages, &bitmap_phys);
    if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
    SetMem((VOID*)(uptr)bitmap_phys, bitmap_pages * PAGE_4K, 0xFF);
    phys_bitmap.buf = (UINT64*)(uptr)bitmap_phys;
    phys_bitmap.bits = total_pages;
	st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, refcount_pages, &refcount_phys);
	if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
	SetMem((VOID*)(uptr)refcount_phys, refcount_pages * PAGE_4K, 0);

    // 3) ????? ??: UEFI? ??? ?? ??
    //    - RAM/??/???: ?? on + NX ??
    //    - MMIO/UC: PCD|PWT + NX
    for (UINT8* cur=(UINT8*)mm, *end=cur+mmSize; cur<end; cur+=descSize) {
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)cur;
        if (d->Type == EfiUnusableMemory || d->Type == EfiReservedMemoryType) continue;
        if (d->Type == EfiConventionalMemory) {
            pb_mark_free_range(&phys_bitmap, (u64)d->PhysicalStart, (u64)d->NumberOfPages * PAGE_4K);
            continue;
        } 
        else if (d->Type == EfiBootServicesCode || d->Type == EfiBootServicesData ||
			d->Type == EfiLoaderCode || d->Type == EfiLoaderData) {
            pb_mark_used_range(&phys_bitmap, (u64)d->PhysicalStart, (u64)d->NumberOfPages * PAGE_4K);
        }
        else if (IsMmioType(d->Type)) {
			continue; // MMIO
		}
        else {
            pb_mark_used_range(&phys_bitmap, (u64)d->PhysicalStart, (u64)d->NumberOfPages * PAGE_4K);
        }

        u64 phys  = (u64)d->PhysicalStart;
        u64 bytes = (u64)d->NumberOfPages * PAGE_4K;
        if (phys >= (1ull<<47)) continue; // 48bit canonical ?? ??

        u64 flags = FlagsFromDesc(d);
        st = MapIdentityRange(pml4_phys, phys, bytes, P | flags);
        if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
    }
    pb_mark_used_range(&phys_bitmap, bitmap_phys, bitmap_pages* PAGE_4K); // ??? ??? ???
	pb_mark_used_range(&phys_bitmap, refcount_phys, refcount_pages * PAGE_4K); // ??? ??? ???

    *out_pml4_phys = pml4_phys;
    // 4) HHDM(physmap) ??: RAM ??? 2MiB? ? (U=0, RW, PS, G, NX)
    {
        EFI_PHYSICAL_ADDRESS hhdm_pdpt_phys;
        st = EnsureHhdmPdpt(pml4_phys, &hhdm_pdpt_phys); 
        if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
        volatile u64* L3 = Tbl(hhdm_pdpt_phys);

        for (UINT8* cur=(UINT8*)mm, *end=cur+mmSize; cur<end; cur+=descSize) {
            EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)cur;
            if (!IsRamType(d->Type)) continue;

            u64 phys = (u64)d->PhysicalStart;
            u64 size = (u64)d->NumberOfPages * PAGE_4K;

            u64 start = AlignDown(phys, PAGE_2M);
            u64 stop  = AlignUp2M(phys + size);

            for (u64 pa=start; pa<stop; pa+=PAGE_2M) {
                // PD ??
                size_t l3i = ((HHDM_BASE + pa) >> 30) & 0x1FF;
                if ((L3[l3i] & P) == 0) {
                    EFI_PHYSICAL_ADDRESS newpd;
                    st = AllocPhysPage(&newpd); if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
                    SetMem((VOID*)(uptr)newpd, PAGE_4K, 0);
                    L3[l3i] = (newpd & PT_ADDR_MASK) | P | RW; // ????: supervisor
                }
                volatile u64* L2 = Tbl(L3[l3i]);
                size_t l2i = ((HHDM_BASE + pa) >> 21) & 0x1FF;

                // ??: 2MiB, ??RW, Global, NX
                L2[l2i] = (pa & PD_2M_MASK) | P | RW | PS | G/* | NX*/;
            }
        }
    }

    gBS->FreePool(mm);
    return EFI_SUCCESS;
}
typedef struct {
    UINT8  type;        // 1 = AHCI/SATA, 2 = NVMe, 3 = USB MSC ...
    UINT16 pci_bus;     // Bus ?? (UEFI? ? ? ? ?? ? ?? 0)
    UINT16 pci_slot;
    UINT16 pci_func;
    UINT32 port_or_ns;  // AHCI ????, NVMe NSID, USB ????
} boot_device_info_t;

typedef struct {
    UINT64 framebufferAddr;
    UINT32 framebufferWidth;
    UINT32 framebufferHeight;
    UINT32 framebufferPitch;
    UINT32 framebufferFormat;
    UINT64 physbm;
	UINT64 refcount;
    UINT64 physbm_size;
    void* rsdp;
    boot_device_info_t bootdev;
} BootInfo;

static EFI_GUID ACPI_20_TABLE_GUID = { 0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81} };
static EFI_GUID ACPI_10_TABLE_GUID = { 0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d} };

void* FindAcpiTable() {
    void* acpi20 = NULL;
    void* acpi10 = NULL;

    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* Entry = &gST->ConfigurationTable[i];

        if (CompareGuid(&Entry->VendorGuid, &ACPI_20_TABLE_GUID)) {
            acpi20 = Entry->VendorTable;
            return acpi20;
        }
        if (CompareGuid(&Entry->VendorGuid, &ACPI_10_TABLE_GUID)) {
            acpi10 = Entry->VendorTable;
        }
    }

    return (acpi20 != NULL) ? acpi20 : acpi10;
}
VOID PrintStatusAndWait(EFI_STATUS Status) {
    if (Status == EFI_SUCCESS) {
        Print(L"[+] Status: EFI_SUCCESS\n");
    } else if (Status == EFI_LOAD_ERROR) {
        Print(L"[-] Status: EFI_LOAD_ERROR\n");
    } else if (Status == EFI_INVALID_PARAMETER) {
        Print(L"[-] Status: EFI_INVALID_PARAMETER\n");
    } else if (Status == EFI_UNSUPPORTED) {
        Print(L"[-] Status: EFI_UNSUPPORTED\n");
    } else if (Status == EFI_BAD_BUFFER_SIZE) {
        Print(L"[-] Status: EFI_BAD_BUFFER_SIZE\n");
    } else if (Status == EFI_BUFFER_TOO_SMALL) {
        Print(L"[-] Status: EFI_BUFFER_TOO_SMALL\n");
    } else if (Status == EFI_NOT_READY) {
        Print(L"[-] Status: EFI_NOT_READY\n");
    } else if (Status == EFI_DEVICE_ERROR) {
        Print(L"[-] Status: EFI_DEVICE_ERROR\n");
    } else if (Status == EFI_WRITE_PROTECTED) {
        Print(L"[-] Status: EFI_WRITE_PROTECTED\n");
    } else if (Status == EFI_OUT_OF_RESOURCES) {
        Print(L"[-] Status: EFI_OUT_OF_RESOURCES\n");
    } else if (Status == EFI_NOT_FOUND) {
        Print(L"[-] Status: EFI_NOT_FOUND\n");
    } else if (Status == EFI_TIMEOUT) {
        Print(L"[-] Status: EFI_TIMEOUT\n");
    } else {
        Print(L"[-] Status: Unknown (0x%lx)\n", Status);
    }

    Print(L"[!] Waiting for 50 seconds...\n");
    gBS->Stall(50000000ULL);
}
#define DevicePathType(a)        (((EFI_DEVICE_PATH_PROTOCOL *)(a))->Type)
#define DevicePathSubType(a)     (((EFI_DEVICE_PATH_PROTOCOL *)(a))->SubType)
#define IsDevicePathEnd(a)       (DevicePathType(a) == END_DEVICE_PATH_TYPE && \
                                  DevicePathSubType(a) == END_ENTIRE_DEVICE_PATH_SUBTYPE)
#define NextDevicePathNode(a)    ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)(a) + \
                                  (((EFI_DEVICE_PATH_PROTOCOL *)(a))->Length[0] + \
                                   ((EFI_DEVICE_PATH_PROTOCOL *)(a))->Length[1] * 256)))
#define BOOTDEV_UNKNOWN 0
#define BOOTDEV_AHCI    1
#define BOOTDEV_NVME    2
#define BOOTDEV_USB     3
#define BOOTDEV_IDE     4
EFI_STATUS FillBootDeviceInfo(EFI_HANDLE DeviceHandle, boot_device_info_t *info) {
    EFI_STATUS Status;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;

    // 1. Device Path ?? (?? ?? ??)
    Status = gBS->HandleProtocol(DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID**)&DevicePath);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    // ???
    info->type       = 0;
    info->pci_bus    = 0;
    info->pci_slot   = 0;
    info->pci_func   = 0;
    info->port_or_ns = 0;

    EFI_DEVICE_PATH_PROTOCOL *Node = DevicePath;

    while (!IsDevicePathEnd(Node)) {
        if (DevicePathType(Node) == MESSAGING_DEVICE_PATH) {
            switch (DevicePathSubType(Node)) {
                case MSG_SATA_DP: {  // SATA / AHCI
                    SATA_DEVICE_PATH *Sata = (SATA_DEVICE_PATH*)Node;
                    info->type       = BOOTDEV_AHCI;
                    info->port_or_ns = Sata->HBAPortNumber;
                    break;
                }
                case 0x03: { // VMware AHCI? ?? ? ? ??
                    ATAPI_DEVICE_PATH *Ata = (ATAPI_DEVICE_PATH*)Node;
                    info->type = BOOTDEV_AHCI;
                    info->port_or_ns = Ata->PrimarySecondary;
                    break;
                }
                case MSG_NVME_NAMESPACE_DP: { // NVMe
                    NVME_NAMESPACE_DEVICE_PATH *Nvme = (NVME_NAMESPACE_DEVICE_PATH*)Node;
                    info->type       = BOOTDEV_NVME;
                    info->port_or_ns = Nvme->NamespaceId;
                    break;
                }
                case MSG_USB_DP: { // USB
                    USB_DEVICE_PATH *Usb = (USB_DEVICE_PATH*)Node;
                    info->type       = BOOTDEV_USB;
                    info->port_or_ns = Usb->ParentPortNumber;
                    break;
                }
            }
        }
        else if (DevicePathType(Node) == HARDWARE_DEVICE_PATH &&
                 DevicePathSubType(Node) == HW_PCI_DP) {
            PCI_DEVICE_PATH *PciNode = (PCI_DEVICE_PATH*)Node;
            // UEFI DevicePath? Bus ??? ?? ? ?? ?? ??
            info->pci_slot = PciNode->Device;
            info->pci_func = PciNode->Function;
        }

        Node = NextDevicePathNode(Node);
    }

    return EFI_SUCCESS;
}
EFI_STATUS FixBootDeviceInfo(EFI_HANDLE ImageHandle, boot_device_info_t *info) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *Loaded = NULL;

    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&Loaded);   // LoadedImage
    if (EFI_ERROR(Status) || !Loaded) return Status;

    EFI_HANDLE BootDev = Loaded->DeviceHandle;

    EFI_DEVICE_PATH_PROTOCOL *DevPath = NULL;
    Status = gBS->HandleProtocol(BootDev, &gEfiDevicePathProtocolGuid, (VOID**)&DevPath);      // DevicePath
    if (EFI_ERROR(Status) || !DevPath) return Status;

    EFI_DEVICE_PATH_PROTOCOL *Walker = DevPath;
    EFI_HANDLE Controller = NULL;
    Status = gBS->LocateDevicePath(&gEfiPciIoProtocolGuid, &Walker, &Controller);              // PCI controller
    if (EFI_ERROR(Status) || !Controller) return EFI_UNSUPPORTED;

    EFI_PCI_IO_PROTOCOL *PciIo = NULL;
    Status = gBS->HandleProtocol(Controller, &gEfiPciIoProtocolGuid, (VOID**)&PciIo);          // PciIo
    if (EFI_ERROR(Status) || !PciIo) return Status;

    UINTN Seg, Bus, Dev, Func;
    Status = PciIo->GetLocation(PciIo, &Seg, &Bus, &Dev, &Func);                                // real location
    if (EFI_ERROR(Status)) return Status;

    info->pci_bus  = (UINT16)Bus;                                                              // store
    info->pci_slot = (UINT16)Dev;
    info->pci_func = (UINT16)Func;

    PCI_TYPE00 Hdr;
    Status = PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0, sizeof(Hdr)/sizeof(UINT32), &Hdr); // config
    if (EFI_ERROR(Status)) return EFI_SUCCESS;

    UINT8 _class = Hdr.Hdr.ClassCode[2];
    UINT8 sub   = Hdr.Hdr.ClassCode[1];
    UINT8 prog  = Hdr.Hdr.ClassCode[0];

    if (info->type == 0) {                                                                      // minimal correction
        if (_class == 0x01 && sub == 0x08)
            info->type = 2;                                                                     // NVMe
        else if (_class == 0x01 && sub == 0x06 && prog == 0x01)
            info->type = 1;                                                                     // AHCI
        else if (_class == 0x0C && sub == 0x03)
            info->type = 3;                                                                     // USB(xHCI)
    }
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    EFI_FILE_PROTOCOL *Root, *File;
    EFI_FILE_INFO *FileInfo;
    UINTN FileInfoSize = 0;
    void* LoadAddress;
    EFI_PHYSICAL_ADDRESS pml4_phys;
    stint();

    // Locate file system from image handle
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }

    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&FileSystem);
    if (EFI_ERROR(Status)) {    
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = BuildIdentityPageTablesFromUefi(&pml4_phys);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to build page tables\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = Root->Open(Root, &File, L"EFI\\Boot\\os.bin", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to open os.bin\n");
        PrintStatusAndWait(Status);
        return Status;
    }

    // Get file size
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &FileInfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
        PrintStatusAndWait(Status);
        return Status;
    }

    FileInfo = (EFI_FILE_INFO*)AllocatePool(FileInfoSize);
    if (!FileInfo) {
        Status = EFI_OUT_OF_RESOURCES;
        PrintStatusAndWait(Status);
        return Status;
    }

    Status = File->GetInfo(File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }

    UINTN FileSize = FileInfo->FileSize;
    FreePool(FileInfo);
    
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES(FileSize), (EFI_PHYSICAL_ADDRESS*)&LoadAddress);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }

    Status = File->Read(File, &FileSize, LoadAddress);
    File->Close(File);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = Root->Close(Root);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
	}
    Status = MapRangeVaToPa(pml4_phys, KERNEL_BASE_VA, (u64)LoadAddress, FileSize, P | RW | G);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to map kernel image\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    // Prepare framebuffer info
    BootInfo* Info;
    Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES(sizeof(BootInfo)), (EFI_PHYSICAL_ADDRESS*)&Info);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = MapRangeVaToPa(pml4_phys, 0xFFFFFFFF00200000ull, (u64)Info, sizeof(BootInfo), P | RW | G);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to map BootInfo structure\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    EFI_GRAPHICS_OUTPUT_PROTOCOL* Graphics;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* ModeInfo;
    UINTN SizeOfInfo;
    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&Graphics);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to locate Graphics Output Protocol\n");
        PrintStatusAndWait(Status);
        return Status;
    }

    Status = Graphics->QueryMode(Graphics, Graphics->Mode->Mode, &SizeOfInfo, &ModeInfo);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to query Graphics Output Protocol mode\n");
        PrintStatusAndWait(Status);
        return Status;
    }

    Info->framebufferAddr   = (UINT64)Graphics->Mode->FrameBufferBase + MMIO_BASE;
    Info->framebufferWidth  = ModeInfo->HorizontalResolution;
    Info->framebufferHeight = ModeInfo->VerticalResolution;
    Info->framebufferPitch  = ModeInfo->PixelsPerScanLine;
    Info->framebufferFormat = ModeInfo->PixelFormat;
    Info->physbm            = (u64)phys_bitmap.buf;
	Info->refcount          = (u64)refcount_phys;
    Info->physbm_size       = (u64)phys_bitmap.bits;
    Info->rsdp              = FindAcpiTable();
    gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    Status = FillBootDeviceInfo(LoadedImage->DeviceHandle, &Info->bootdev);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to get boot device info\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = MapRangeVaToPa(pml4_phys, Info->framebufferAddr, Graphics->Mode->FrameBufferBase, Info->framebufferHeight * Info->framebufferPitch * 4, P | RW | G | PCD | PWT);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to map framebuffer memory\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    UINT64 stack_top;
    Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES(0x10000), (EFI_PHYSICAL_ADDRESS*)&stack_top);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to allocate stack\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    Status = MapRangeVaToPa(pml4_phys,
                               0xFFFFFFFFFFF00000ull,  // VA (??? ??? ????)
                               stack_top,  // PA (?? ????)
                               0x10000,             // ?? 64KiB
                               P | RW | G);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to map stack\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    UINTN MapSize = 0, MapKey, DescSize;
    UINT32 DescVersion;
    gBS->GetMemoryMap(&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
    MapSize += 2 * DescSize;
    VOID *Map = AllocatePool(MapSize);
    Status = gBS->GetMemoryMap(&MapSize, (EFI_MEMORY_DESCRIPTOR*)Map, &MapKey, &DescSize, &DescVersion);
    Status = gBS->ExitBootServices(ImageHandle, MapKey);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }
    load_gdt((void*)pml4_phys);
    // Jump to OS
    jump_to_address((void*)(0xFFFFFFFFFFF00000ull + 0x10000),(void*)KERNEL_BASE_VA);
    while (1) {
        hlt_loop();
    }
    return EFI_SUCCESS;
}

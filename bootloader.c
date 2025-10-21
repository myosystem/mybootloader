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

#include <Guid/FileInfo.h>

#include <Register/Intel/ArchitecturalMsr.h>

extern void jump_to_address(void* stack_top,void* addr);
extern void load_gdt(void* pml4_phys);
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
#define PT_ADDR_MASK 0x000FFFFFFFFFF000ull  // 하위 12비트 플래그 마스크
#define PD_2M_MASK   0x000FFFFFFFE00000ull  // 2MiB 대페이지 물리주소 마스크
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
// UEFI에서 4KiB 물리 페이지 하나 할당하고 0으로 클리어
static EFI_STATUS AllocPhysPage(EFI_PHYSICAL_ADDRESS* OutPhys) {
    EFI_STATUS st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, OutPhys);
    if (EFI_ERROR(st)) return st;
    pb_mark_used_range(&phys_bitmap, *OutPhys, PAGE_4K);
    // 대부분의 펌웨어가 저주소 아이덴티티라 물리=가상 캐스팅으로 접근 가능
    SetMem((VOID*)(uptr)(*OutPhys), PAGE_4K, 0);
    return EFI_SUCCESS;
}

static inline volatile u64* PhysToPtr(EFI_PHYSICAL_ADDRESS phys) {
    return (volatile u64*)(uptr)phys; // 아이덴티티 가정
}
static inline volatile u64* Tbl(u64 phys){ return PhysToPtr(phys & PT_ADDR_MASK); }

// 인덱스 추출 (IA-32e 4레벨)
static inline u64 IdxPml4(u64 va) { return (va >> 39) & 0x1FF; }
static inline u64 IdxPdpt(u64 va) { return (va >> 30) & 0x1FF; }
static inline u64 IdxPd  (u64 va) { return (va >> 21) & 0x1FF; }
static inline u64 IdxPt  (u64 va) { return (va >> 12) & 0x1FF; }

// PML4 -> PDPT 확보
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

// PDPT -> PD 확보
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

// PD -> PT 확보 (4KiB용). 이미 2MiB 대페이지면 충돌.
static EFI_STATUS EnsurePt(EFI_PHYSICAL_ADDRESS pml4_phys, u64 va, EFI_PHYSICAL_ADDRESS* out_pt_phys) {
    EFI_STATUS st;
    EFI_PHYSICAL_ADDRESS pd_phys;
    st = EnsurePd(pml4_phys, va, &pd_phys); if (EFI_ERROR(st)) return st;
    volatile u64* pd = PhysToPtr(pd_phys);
    u64 i = IdxPd(va);
    if ((pd[i] & P) == 0) {
        EFI_PHYSICAL_ADDRESS newp;
        st = AllocPhysPage(&newp); if (EFI_ERROR(st)) return st;
        pd[i] = (newp & PT_ADDR_MASK) | P | RW; // 하위 PT를 가리킴
    } else if (pd[i] & PS) {
        return EFI_DEVICE_ERROR; // 이미 2MiB 매핑 존재
    }
    *out_pt_phys = (pd[i] & PT_ADDR_MASK);
    return EFI_SUCCESS;
}

// 2MiB 매핑
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

// 4KiB 매핑
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
// 범위를 아이덴티티로 매핑 (앞/뒤 가장자리는 4KiB, 가운데는 2MiB)
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
    EFI_STATUS st;
    u64 cur_va = va;
    u64 cur_pa = pa;
    u64 end_va = va + bytes; // bytes가 클 때 overflow 방지하려면 체크 추가 가능

    // 1) 앞 가장자리: 2MiB 정렬이 서로 맞을 때까지 4KiB로
    //    (둘 다 2MiB 경계에 있어야 2MiB 대페이지 가능)
    while (cur_va < end_va) {
        // 조건이 되면 곧바로 2MiB 구간으로 넘어간다
        if ( ((cur_va | cur_pa) % PAGE_2M == 0) && ((end_va - cur_va) >= PAGE_2M) )
            break;

        // 2MiB까지 남은 거리 중 작은 쪽, 또는 남은 전체를 4KiB 단위로 메우자
        u64 next2m_va = AlignUp(cur_va, PAGE_2M);
        u64 next2m_pa = AlignUp(cur_pa, PAGE_2M);
        u64 edge      = next2m_va - cur_va;
        u64 edge_pa   = next2m_pa - cur_pa;
        if (edge_pa < edge) edge = edge_pa;
        if (edge == 0) edge = PAGE_4K;              // 최소 한 페이지
        if (edge > (end_va - cur_va)) edge = end_va - cur_va;

        // 4KiB로 edge 구간 매핑
        u64 to_map = edge & ~(PAGE_4K - 1);         // 4KiB 배수로 내림
        if (to_map == 0) to_map = PAGE_4K;
        for (u64 off = 0; off < to_map; off += PAGE_4K) {
            st = Map4K(pml4_phys, cur_va + off, cur_pa + off, flags);
            if (EFI_ERROR(st)) return st;
        }
        cur_va += to_map;
        cur_pa += to_map;
    }

    // 2) 가운데: 2MiB 블록으로 쾅쾅
    while ((end_va - cur_va) >= PAGE_2M) {
        // 이 시점에는 cur_va/cur_pa가 둘 다 2MiB 정렬돼 있어야 한다
        if ( (cur_va % PAGE_2M) || (cur_pa % PAGE_2M) ) break;
        st = Map2M(pml4_phys, cur_va, cur_pa, flags);
        if (EFI_ERROR(st)) return st;
        cur_va += PAGE_2M;
        cur_pa += PAGE_2M;
    }

    // 3) 뒤 가장자리: 남은 건 4KiB로 마무리
    while (cur_va < end_va) {
        st = Map4K(pml4_phys, cur_va, cur_pa, flags);
        if (EFI_ERROR(st)) return st;
        cur_va += PAGE_4K;
        cur_pa += PAGE_4K;
    }

    return EFI_SUCCESS;
}
// UEFI 메모리 속성 -> 페이지 플래그 (단순화)
// MMIO/UC는 비캐시(PCD|PWT), 그 외는 캐시 사용.
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
           t==EfiACPIReclaimMemory; // 선택
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
// 공개 API: 메모리맵을 읽어 UEFI가 인지한 모든 영역을 아이덴티티 매핑
EFI_STATUS BuildIdentityPageTablesFromUefi(EFI_PHYSICAL_ADDRESS* out_pml4_phys) {
    EFI_STATUS st;

    // 1) 새 PML4
    EFI_PHYSICAL_ADDRESS pml4_phys = 0;
    st = AllocPhysPage(&pml4_phys); if (EFI_ERROR(st)) return st;

    // 2) 메모리맵 가져오기
    UINTN mmSize = 0, mapKey = 0, descSize = 0; UINT32 descVer = 0;
    st = gBS->GetMemoryMap(&mmSize, NULL, &mapKey, &descSize, &descVer);
    if (st != EFI_BUFFER_TOO_SMALL) return st;

    mmSize += 2 * descSize; // 여유
    EFI_MEMORY_DESCRIPTOR* mm = NULL;
    st = gBS->AllocatePool(EfiLoaderData, mmSize, (VOID**)&mm);
    if (EFI_ERROR(st)) return st;

    st = gBS->GetMemoryMap(&mmSize, mm, &mapKey, &descSize, &descVer);
    if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }

    u64 top = 0;
    for(u64 off = 0; off < mmSize; off += descSize){
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)mm + off);
        //Print(L"Type %u, start=%lx, pages=%lx\n", d->Type, d->PhysicalStart, d->NumberOfPages);
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
    EFI_PHYSICAL_ADDRESS bitmap_phys;
    Print(L"[+] Top of used phys memory: 0x%lx, total pages: %lu, bitmap size: %lu bytes (%lu pages)\n",
          top, total_pages, bitmap_size, bitmap_pages);
    st = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, bitmap_pages, &bitmap_phys);
    if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
    SetMem((VOID*)(uptr)bitmap_phys, bitmap_pages * PAGE_4K, 0xFF);
    phys_bitmap.buf = (UINT64*)(uptr)bitmap_phys;
    phys_bitmap.bits = total_pages;
    pb_mark_used_range(&phys_bitmap, 0, bitmap_pages * PAGE_4K); // 비트맵 영역은 사용중
    // 3) 아이덴티티 매핑: UEFI가 인지한 모든 영역
    //    - RAM/코드/데이터: 캐시 on + NX 기본
    //    - MMIO/UC: PCD|PWT + NX
    for (UINT8* cur=(UINT8*)mm, *end=cur+mmSize; cur<end; cur+=descSize) {
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)cur;
        if (d->Type == EfiUnusableMemory) continue;
        if (d->Type == EfiConventionalMemory) {
            pb_mark_free_range(&phys_bitmap, (u64)d->PhysicalStart, (u64)d->NumberOfPages * PAGE_4K);
        } else {
            pb_mark_used_range(&phys_bitmap, (u64)d->PhysicalStart, (u64)d->NumberOfPages * PAGE_4K);
        }

        u64 phys  = (u64)d->PhysicalStart;
        u64 bytes = (u64)d->NumberOfPages * PAGE_4K;
        if (phys >= (1ull<<47)) continue; // 48bit canonical 밖은 스킵

        u64 flags = FlagsFromDesc(d);
        st = MapIdentityRange(pml4_phys, phys, bytes, P | flags);
        if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
    }

    *out_pml4_phys = pml4_phys;
    // 4) HHDM(physmap) 설치: RAM 타입만 2MiB로 쫙 (U=0, RW, PS, G, NX)
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
                // PD 보장
                size_t l3i = ((HHDM_BASE + pa) >> 30) & 0x1FF;
                if ((L3[l3i] & P) == 0) {
                    EFI_PHYSICAL_ADDRESS newpd;
                    st = AllocPhysPage(&newpd); if (EFI_ERROR(st)) { gBS->FreePool(mm); return st; }
                    SetMem((VOID*)(uptr)newpd, PAGE_4K, 0);
                    L3[l3i] = (newpd & PT_ADDR_MASK) | P | RW; // 상위레벨: supervisor
                }
                volatile u64* L2 = Tbl(L3[l3i]);
                size_t l2i = ((HHDM_BASE + pa) >> 21) & 0x1FF;

                // 리프: 2MiB, 커널RW, Global, NX
                L2[l2i] = (pa & PD_2M_MASK) | P | RW | PS | G/* | NX*/;
            }
        }
    }

    gBS->FreePool(mm);
    return EFI_SUCCESS;
}
typedef struct {
    UINT8  type;        // 1 = AHCI/SATA, 2 = NVMe, 3 = USB MSC ...
    UINT16 pci_bus;     // Bus 번호 (UEFI는 안 줄 수 있음 → 보통 0)
    UINT16 pci_slot;
    UINT16 pci_func;
    UINT32 port_or_ns;  // AHCI 포트번호, NVMe NSID, USB 포트번호
} boot_device_info_t;

typedef struct {
    UINT64 framebufferAddr;
    UINT32 framebufferWidth;
    UINT32 framebufferHeight;
    UINT32 framebufferPitch;
    UINT32 framebufferFormat;
    UINT64 physbm;
    UINT64 physbm_size;
    void* rsdp;
    boot_device_info_t bootdev;
} BootInfo;

static EFI_GUID ACPI_20_TABLE_GUID = { 0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81} };
static EFI_GUID ACPI_10_TABLE_GUID = { 0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d} };

void* FindAcpiTable() {
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *Entry = &gST->ConfigurationTable[i];
        if (CompareGuid(&Entry->VendorGuid, &ACPI_20_TABLE_GUID) ||
            CompareGuid(&Entry->VendorGuid, &ACPI_10_TABLE_GUID)) {
            return Entry->VendorTable;
        }
    }
    Print(L"ACPI Table Not Found\n");
    return NULL;
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

EFI_STATUS FillBootDeviceInfo(EFI_HANDLE DeviceHandle, boot_device_info_t *info) {
    EFI_STATUS Status;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;

    // 1. Device Path 얻기 (장치 종류 판별)
    Status = gBS->HandleProtocol(DeviceHandle, &gEfiDevicePathProtocolGuid, (VOID**)&DevicePath);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    // 초기화
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
                    info->type       = 1;
                    info->port_or_ns = Sata->HBAPortNumber;
                    break;
                }
                case MSG_NVME_NAMESPACE_DP: { // NVMe
                    NVME_NAMESPACE_DEVICE_PATH *Nvme = (NVME_NAMESPACE_DEVICE_PATH*)Node;
                    info->type       = 2;
                    info->port_or_ns = Nvme->NamespaceId;
                    break;
                }
                case MSG_USB_DP: { // USB
                    USB_DEVICE_PATH *Usb = (USB_DEVICE_PATH*)Node;
                    info->type       = 3;
                    info->port_or_ns = Usb->ParentPortNumber;
                    break;
                }
            }
        }
        else if (DevicePathType(Node) == HARDWARE_DEVICE_PATH &&
                 DevicePathSubType(Node) == HW_PCI_DP) {
            PCI_DEVICE_PATH *PciNode = (PCI_DEVICE_PATH*)Node;
            // UEFI DevicePath는 Bus 번호는 명시 안 해줄 수도 있음
            info->pci_slot = PciNode->Device;
            info->pci_func = PciNode->Function;
        }

        Node = NextDevicePathNode(Node);
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
    Print(L"[+] Bootloader started\n");

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
    Print(L"[+] Building page tables...\n");
    Status = BuildIdentityPageTablesFromUefi(&pml4_phys);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to build page tables\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    Print(L"[+] Page tables built at 0x%lx\n", pml4_phys);
    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }
    Print(L"[+] Loading kernel image...\n");
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

    FileInfo = AllocatePool(FileInfoSize);
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
    Info->physbm           = (u64)phys_bitmap.buf;
    Info->physbm_size      = (u64)phys_bitmap.bits;
    Info->rsdp              = FindAcpiTable();
    Status = FillBootDeviceInfo(LoadedImage->DeviceHandle, &Info->bootdev);
    if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to get boot device info\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    Print(L"[+] Boot device type: %u, PCI %u:%u:%u, port/ns: %u\n",
          Info->bootdev.type,
          Info->bootdev.pci_bus,
          Info->bootdev.pci_slot,
          Info->bootdev.pci_func,
          Info->bootdev.port_or_ns);
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
                               0xFFFFFFFFFFF00000ull,  // VA (스택을 쓰려는 가상주소)
                               stack_top,  // PA (실제 물리주소)
                               0x10000,             // 길이 64KiB
                               P | RW | G);
                               if (EFI_ERROR(Status)) {
        Print(L"[-] Failed to map stack\n");
        PrintStatusAndWait(Status);
        return Status;
    }
    Print(L"[+] Framebuffer: 0x%lx (%ux%u)\n", Info->framebufferAddr, Info->framebufferWidth, Info->framebufferHeight);
    u64 pml4e = ((u64*)pml4_phys)[510];
    u64 pdpt_phys = pml4e & PT_ADDR_MASK;
    u64 pdpte0 = ((u64*)pdpt_phys)[0];
    u64 pd_phys = pdpte0 & PT_ADDR_MASK;
    u64 pde2 = ((u64*)pd_phys)[2];
    Print(L"[+] pml4_phys: 0x%lx\n", pml4_phys);
    Print(L"PML4[510] = %lx\n", pml4e);
    Print(L"PDPT[0] = %lx\n", pdpte0);
    Print(L" PD[2] = %lx\n", pde2);
    //gBS->Stall(5000000ULL);
    //CpuSleep();
    // Exit boot services
    UINTN MapSize = 0, MapKey, DescSize;
    UINT32 DescVersion;
    gBS->GetMemoryMap(&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
    MapSize += 2 * DescSize;
    VOID *Map = AllocatePool(MapSize);
    Status = gBS->GetMemoryMap(&MapSize, Map, &MapKey, &DescSize, &DescVersion);
    Status = gBS->ExitBootServices(ImageHandle, MapKey);
    if (EFI_ERROR(Status)) {
        PrintStatusAndWait(Status);
        return Status;
    }

    //*(BootInfo*)(UINTN)0x200000 = *Info;
    load_gdt((void*)pml4_phys);
    // Jump to OS
    jump_to_address((void*)(0xFFFFFFFFFFF00000ull + 0x10000),(void*)KERNEL_BASE_VA);
    return EFI_SUCCESS;
}

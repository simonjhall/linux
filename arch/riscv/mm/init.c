// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/initrd.h>
#include <linux/swap.h>
#include <linux/sizes.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/set_memory.h>
#include <linux/dma-map-ops.h>

#include <asm/fixmap.h>
#include <asm/tlbflush.h>
#include <asm/sections.h>
#include <asm/soc.h>
#include <asm/io.h>
#include <asm/ptdump.h>
#include <asm/numa.h>

#include "../kernel/head.h"

unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)]
							__page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

extern char _start[];
#define DTB_EARLY_BASE_VA      PGDIR_SIZE
void *dtb_early_va __initdata;
uintptr_t dtb_early_pa __initdata;

struct pt_alloc_ops {
	pte_t *(*get_pte_virt)(phys_addr_t pa);
	phys_addr_t (*alloc_pte)(uintptr_t va);
#ifndef __PAGETABLE_PMD_FOLDED
	pmd_t *(*get_pmd_virt)(phys_addr_t pa);
	phys_addr_t (*alloc_pmd)(uintptr_t va);
#endif
};

static phys_addr_t dma32_phys_limit __ro_after_init;

static void __init zone_sizes_init(void)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES] = { 0, };

#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(dma32_phys_limit);
#endif
	max_zone_pfns[ZONE_NORMAL] = max_low_pfn;

	free_area_init(max_zone_pfns);
}

static void setup_zero_page(void)
{
	memset((void *)empty_zero_page, 0, PAGE_SIZE);
}

#if defined(CONFIG_MMU) && defined(CONFIG_DEBUG_VM)
static inline void print_mlk(char *name, unsigned long b, unsigned long t)
{
	pr_notice("%12s : 0x%08lx - 0x%08lx   (%4ld kB)\n", name, b, t,
		  (((t) - (b)) >> 10));
}

static inline void print_mlm(char *name, unsigned long b, unsigned long t)
{
	pr_notice("%12s : 0x%08lx - 0x%08lx   (%4ld MB)\n", name, b, t,
		  (((t) - (b)) >> 20));
}

static void print_vm_layout(void)
{
	pr_notice("Virtual kernel memory layout:\n");
	print_mlk("fixmap", (unsigned long)FIXADDR_START,
		  (unsigned long)FIXADDR_TOP);
	print_mlm("pci io", (unsigned long)PCI_IO_START,
		  (unsigned long)PCI_IO_END);
	print_mlm("vmemmap", (unsigned long)VMEMMAP_START,
		  (unsigned long)VMEMMAP_END);
	print_mlm("vmalloc", (unsigned long)VMALLOC_START,
		  (unsigned long)VMALLOC_END);
	print_mlm("lowmem", (unsigned long)PAGE_OFFSET,
		  (unsigned long)high_memory);
}
#else
static void print_vm_layout(void) { }
#endif /* CONFIG_DEBUG_VM */

void __init mem_init(void)
{
#ifdef CONFIG_FLATMEM
	BUG_ON(!mem_map);
#endif /* CONFIG_FLATMEM */

	high_memory = (void *)(__va(PFN_PHYS(max_low_pfn)));
	memblock_free_all();

	mem_init_print_info(NULL);
	print_vm_layout();
}

void __init setup_bootmem(void)
{
	phys_addr_t vmlinux_end = __pa_symbol(&_end);
	phys_addr_t vmlinux_start = __pa_symbol(&_start);
	phys_addr_t dram_end = memblock_end_of_DRAM();
	phys_addr_t max_mapped_addr = __pa(~(ulong)0);

	/* The maximal physical memory size is -PAGE_OFFSET. */
	memblock_enforce_memory_limit(-PAGE_OFFSET);

	/* Reserve from the start of the kernel to the end of the kernel */
	memblock_reserve(vmlinux_start, vmlinux_end - vmlinux_start);

	/*
	 * memblock allocator is not aware of the fact that last 4K bytes of
	 * the addressable memory can not be mapped because of IS_ERR_VALUE
	 * macro. Make sure that last 4k bytes are not usable by memblock
	 * if end of dram is equal to maximum addressable memory.
	 */
	if (max_mapped_addr == (dram_end - 1))
		memblock_set_current_limit(max_mapped_addr - 4096);

	max_pfn = PFN_DOWN(dram_end);
	max_low_pfn = max_pfn;
	dma32_phys_limit = min(4UL * SZ_1G, (unsigned long)PFN_PHYS(max_low_pfn));
	set_max_mapnr(max_low_pfn - ARCH_PFN_OFFSET);

	reserve_initrd_mem();
	/*
	 * If DTB is built in, no need to reserve its memblock.
	 * Otherwise, do reserve it but avoid using
	 * early_init_fdt_reserve_self() since __pa() does
	 * not work for DTB pointers that are fixmap addresses
	 */
	if (!IS_ENABLED(CONFIG_BUILTIN_DTB))
		memblock_reserve(dtb_early_pa, fdt_totalsize(dtb_early_va));

	early_init_fdt_scan_reserved_mem();
	dma_contiguous_reserve(dma32_phys_limit);
	memblock_allow_resize();
}

#ifdef CONFIG_MMU
static struct pt_alloc_ops pt_ops;

unsigned long va_pa_offset;
EXPORT_SYMBOL(va_pa_offset);
unsigned long pfn_base;
EXPORT_SYMBOL(pfn_base);

pgd_t swapper_pg_dir[PTRS_PER_PGD] __page_aligned_bss;
pgd_t trampoline_pg_dir[PTRS_PER_PGD] __page_aligned_bss;
pte_t fixmap_pte[PTRS_PER_PTE] __page_aligned_bss;

pgd_t early_pg_dir[PTRS_PER_PGD] __initdata __aligned(PAGE_SIZE);

void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *ptep;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

	ptep = &fixmap_pte[pte_index(addr)];

	if (pgprot_val(prot))
		set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, prot));
	else
		pte_clear(&init_mm, addr, ptep);
	local_flush_tlb_page(addr);
}

static inline pte_t *__init get_pte_virt_early(phys_addr_t pa)
{
	return (pte_t *)((uintptr_t)pa);
}

static inline pte_t *__init get_pte_virt_fixmap(phys_addr_t pa)
{
	clear_fixmap(FIX_PTE);
	return (pte_t *)set_fixmap_offset(FIX_PTE, pa);
}

static inline pte_t *get_pte_virt_late(phys_addr_t pa)
{
	return (pte_t *) __va(pa);
}

static inline phys_addr_t __init alloc_pte_early(uintptr_t va)
{
	/*
	 * We only create PMD or PGD early mappings so we
	 * should never reach here with MMU disabled.
	 */
	BUG();
}

static inline phys_addr_t __init alloc_pte_fixmap(uintptr_t va)
{
	return memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
}

static phys_addr_t alloc_pte_late(uintptr_t va)
{
	unsigned long vaddr;

	vaddr = __get_free_page(GFP_KERNEL);
	if (!vaddr || !pgtable_pte_page_ctor(virt_to_page(vaddr)))
		BUG();
	return __pa(vaddr);
}

static void __init create_pte_mapping(pte_t *ptep,
				      uintptr_t va, phys_addr_t pa,
				      phys_addr_t sz, pgprot_t prot)
{
	uintptr_t pte_idx = pte_index(va);

	BUG_ON(sz != PAGE_SIZE);

	if (pte_none(ptep[pte_idx]))
		ptep[pte_idx] = pfn_pte(PFN_DOWN(pa), prot);
}

#ifndef __PAGETABLE_PMD_FOLDED

pmd_t trampoline_pmd[PTRS_PER_PMD] __page_aligned_bss;
pmd_t fixmap_pmd[PTRS_PER_PMD] __page_aligned_bss;
pmd_t early_pmd[PTRS_PER_PMD] __initdata __aligned(PAGE_SIZE);
pmd_t early_dtb_pmd[PTRS_PER_PMD] __initdata __aligned(PAGE_SIZE);

static pmd_t *__init get_pmd_virt_early(phys_addr_t pa)
{
	/* Before MMU is enabled */
	return (pmd_t *)((uintptr_t)pa);
}

static pmd_t *__init get_pmd_virt_fixmap(phys_addr_t pa)
{
	clear_fixmap(FIX_PMD);
	return (pmd_t *)set_fixmap_offset(FIX_PMD, pa);
}

static pmd_t *get_pmd_virt_late(phys_addr_t pa)
{
	return (pmd_t *) __va(pa);
}

static phys_addr_t __init alloc_pmd_early(uintptr_t va)
{
	BUG_ON((va - PAGE_OFFSET) >> PGDIR_SHIFT);

	return (uintptr_t)early_pmd;
}

static phys_addr_t __init alloc_pmd_fixmap(uintptr_t va)
{
	return memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
}

static phys_addr_t alloc_pmd_late(uintptr_t va)
{
	unsigned long vaddr;

	vaddr = __get_free_page(GFP_KERNEL);
	BUG_ON(!vaddr);
	return __pa(vaddr);
}

static void __init create_pmd_mapping(pmd_t *pmdp,
				      uintptr_t va, phys_addr_t pa,
				      phys_addr_t sz, pgprot_t prot)
{
	pte_t *ptep;
	phys_addr_t pte_phys;
	uintptr_t pmd_idx = pmd_index(va);

	if (sz == PMD_SIZE) {
		if (pmd_none(pmdp[pmd_idx]))
			pmdp[pmd_idx] = pfn_pmd(PFN_DOWN(pa), prot);
		return;
	}

	if (pmd_none(pmdp[pmd_idx])) {
		pte_phys = pt_ops.alloc_pte(va);
		pmdp[pmd_idx] = pfn_pmd(PFN_DOWN(pte_phys), PAGE_TABLE);
		ptep = pt_ops.get_pte_virt(pte_phys);
		memset(ptep, 0, PAGE_SIZE);
	} else {
		pte_phys = PFN_PHYS(_pmd_pfn(pmdp[pmd_idx]));
		ptep = pt_ops.get_pte_virt(pte_phys);
	}

	create_pte_mapping(ptep, va, pa, sz, prot);
}

#define pgd_next_t		pmd_t
#define alloc_pgd_next(__va)	pt_ops.alloc_pmd(__va)
#define get_pgd_next_virt(__pa)	pt_ops.get_pmd_virt(__pa)
#define create_pgd_next_mapping(__nextp, __va, __pa, __sz, __prot)	\
	create_pmd_mapping(__nextp, __va, __pa, __sz, __prot)
#define fixmap_pgd_next		fixmap_pmd
#else
#define pgd_next_t		pte_t
#define alloc_pgd_next(__va)	pt_ops.alloc_pte(__va)
#define get_pgd_next_virt(__pa)	pt_ops.get_pte_virt(__pa)
#define create_pgd_next_mapping(__nextp, __va, __pa, __sz, __prot)	\
	create_pte_mapping(__nextp, __va, __pa, __sz, __prot)
#define fixmap_pgd_next		fixmap_pte
#endif

void __init create_pgd_mapping(pgd_t *pgdp,
				      uintptr_t va, phys_addr_t pa,
				      phys_addr_t sz, pgprot_t prot)
{
	pgd_next_t *nextp;
	phys_addr_t next_phys;
	uintptr_t pgd_idx = pgd_index(va);

	if (sz == PGDIR_SIZE) {
		if (pgd_val(pgdp[pgd_idx]) == 0)
			pgdp[pgd_idx] = pfn_pgd(PFN_DOWN(pa), prot);
		return;
	}

	if (pgd_val(pgdp[pgd_idx]) == 0) {
		next_phys = alloc_pgd_next(va);
		pgdp[pgd_idx] = pfn_pgd(PFN_DOWN(next_phys), PAGE_TABLE);
		nextp = get_pgd_next_virt(next_phys);
		memset(nextp, 0, PAGE_SIZE);
	} else {
		next_phys = PFN_PHYS(_pgd_pfn(pgdp[pgd_idx]));
		nextp = get_pgd_next_virt(next_phys);
	}

	create_pgd_next_mapping(nextp, va, pa, sz, prot);
}

static uintptr_t __init best_map_size(phys_addr_t base, phys_addr_t size)
{
	/* Upgrade to PMD_SIZE mappings whenever possible */
	if ((base & (PMD_SIZE - 1)) || (size & (PMD_SIZE - 1)))
		return PAGE_SIZE;

	return PMD_SIZE;
}

/*
 * setup_vm() is called from head.S with MMU-off.
 *
 * Following requirements should be honoured for setup_vm() to work
 * correctly:
 * 1) It should use PC-relative addressing for accessing kernel symbols.
 *    To achieve this we always use GCC cmodel=medany.
 * 2) The compiler instrumentation for FTRACE will not work for setup_vm()
 *    so disable compiler instrumentation when FTRACE is enabled.
 *
 * Currently, the above requirements are honoured by using custom CFLAGS
 * for init.o in mm/Makefile.
 */

#ifndef __riscv_cmodel_medany
#error "setup_vm() is called from head.S before relocate so it should not use absolute addressing."
#endif

enum TrapNonInterruptCause
{
	TrapInsAddrMisaligned	=	0,
	TrapInsAccFault			=	1,
	TrapIllegalIns			=	2,
	TrapBreakpoint			=	3,
	TrapLoadMisaligned		=	4,
	TrapLoadFault			=	5,
	TrapStoreMisaligned		=	6,
	TrapStoreFault			=	7,
	TrapUserCall			=	8,
	TrapSuperCall			=	9,
	TrapMachineCall			=	11,
	TrapInsPageFault		=	12,
	TrapLoadPageFault		=	13,
	TrapStorePageFault		=	15,

	TrapInstTlbMiss			=	24,
	TrapDataTlbMiss			=	25,
};

enum TrapInterruptCause
{
	TrapSSoftInt			=	1,
	TrapMSoftInt			=	3,
	TrapSTimerInt			=	5,
	TrapMTimerInt			=	7,
	TrapSExtInt				=	9,
	TrapMExtInt				=	11,
};

#ifdef CONFIG_32BIT
static const uint64_t kMcauseInterrupt = 1 << 31;
static const uint64_t kMcauseNotInterrupt = 0 << 31;
static const uint64_t kMcauseInterruptMask = 1 << 31;
#else
static const uint64_t kMcauseInterrupt = 1ull << 63;
static const uint64_t kMcauseNotInterrupt = 0ull << 63;
static const uint64_t kMcauseInterruptMask = 1ull << 63;
#endif
//////////////////////

static const uint64_t kRv32SatpModeShift = 31;
static const uint64_t kRv32SatpPpnMask = (1ull << 22) - 1;
//////////////////////

static const uint64_t kRv64SatpModeShift = 60;
static const uint64_t kRv64SatpPpnMask = (1ull << 44) - 1;
//////////////////////

static const uint64_t kSv32VaMask = (1ull << 32) - 1;
static const uint64_t kSv32VaVpnWidth = 10;		//the size of an individual VPN segment
static const uint64_t kSv32VaVpnMask = (1ull << kSv32VaVpnWidth) - 1;

static const uint64_t kSv32PteSizeBytes = 4;

static const uint64_t kSv32PtePpn0Shift = 10;
static const uint64_t kSv32PtePpn1Shift = 20;
//////////////////////

static const uint64_t kSv39VaMask = (1ull << 39) - 1;
static const uint64_t kSv39VaVpnWidth = 9;		//the size of an individual VPN segment
static const uint64_t kSv39VaVpnMask = (1ull << kSv39VaVpnWidth) - 1;

static const uint64_t kSv39PteSizeBytes = 8;

static const uint64_t kSv39PtePpn0Shift = 10;
static const uint64_t kSv39PtePpn1Shift = 19;
static const uint64_t kSv39PtePpn2Shift = 28;
//////////////////////

static const uint64_t kSvCommonVaPageWidth = 12;
static const uint64_t kSvCommonVaPageMask = (1 << kSvCommonVaPageWidth) - 1;

static const uint64_t kSvCommonPteVMask = 1 << 0;
static const uint64_t kSvCommonPteRMask = 1 << 1;
static const uint64_t kSvCommonPteWMask = 1 << 2;
static const uint64_t kSvCommonPteXMask = 1 << 3;
static const uint64_t kSvCommonPteUMask = 1 << 4;
static const uint64_t kSvCommonPteGMask = 1 << 5;
static const uint64_t kSvCommonPteAMask = 1 << 6;
static const uint64_t kSvCommonPteDMask = 1 << 7;

static const uint64_t kSvCommonPteXWRMask = kSvCommonPteXMask | kSvCommonPteWMask | kSvCommonPteRMask;
static const uint64_t kSvCommonPteDAGUXWRVMask = 0xff;

//////////////////////

static const uint64_t kSatpModeBare = 0;
static const uint64_t kSatpModeSv32 = 1;
static const uint64_t kSatpModeSv39 = 8;

//////////////////////

static const unsigned int kTlbInvalid	= 0;
static const unsigned int kTlbRegular	= 1;
static const unsigned int kTlbMega		= 2;
static const unsigned int kTlbGiga		= 3;

//////////////////////

uint64_t reservation_addr_pa;
uint32_t tlb_entry;				//rolling index of tlb index

//////////////////////

// #define ILL_COUNT

#ifdef ILL_COUNT
#define ILL_INC(x) ((x)++)

//debug counters
static unsigned int ill_system_csrrs = 0;
static unsigned int ill_op32_divw = 0;
static unsigned int ill_op32_divuw = 0;
static unsigned int ill_op32_remw = 0;
static unsigned int ill_op32_remuw = 0;
static unsigned int ill_op_div = 0;
static unsigned int ill_op_divu = 0;
static unsigned int ill_op_rem = 0;
static unsigned int ill_op_remu = 0;
static unsigned int ill_amo_amoswap = 0;
static unsigned int ill_amo_amoadd = 0;
static unsigned int ill_amo_amoxor = 0;
static unsigned int ill_amo_amoand = 0;
static unsigned int ill_amo_amoor = 0;
static unsigned int ill_amo_lr = 0;
static unsigned int ill_amo_sc = 0;

#else
#define ILL_INC(x)
#endif

static unsigned int ecall = 0;

//////////////////////

#if defined CONFIG_VM68K || defined CONFIG_SOC_VMRISCV
#define UART_DATA_OFFSET 0x580
#define UART_STATUS_OFFSET 0x584
#endif

//UART 1
#if defined CONFIG_REDUX || defined CONFIG_SOC_REDUX
#define UART_DATA_OFFSET 0x588
#define UART_STATUS_OFFSET 0x58C
#endif

static void ft245_put_char(char c)
{
	volatile char *pData = (volatile char *)(0x1000000 + UART_DATA_OFFSET);
	volatile char *pState = (volatile char *)(0x1000000 + UART_STATUS_OFFSET);

	while (*pState & (1 << 3))			//no space
		;
		
	*pData = c;
}

static void ft245_put_string(const char *pString)
{
	while (*pString)
		ft245_put_char(*pString++);
}

static void ft245_put_hex_num(unsigned long n)
{
#ifdef CONFIG_32BIT
	const int start = 7;
#elif CONFIG_64BIT
	const int start = 15;
#endif
	int count;
	for (count = start; count >= 0; count--)
	{
		unsigned long val = (n >> (count * 4)) & 0xf;
		if (val < 10)
			ft245_put_char('0' + val);
		else
			ft245_put_char('a' + val - 10);
	}
}


//////////////////////

asmlinkage void _m_write_tlb(uint32_t id, uint32_t entry, unsigned long vpn, unsigned long ppn, uint32_t daguxwrv, uint32_t size)
{
	const unsigned int kTlbEntriesBits = 2;
	const unsigned int kTlbEntries = 1 << kTlbEntriesBits;
	const unsigned int kTlbEntriesMask = kTlbEntries - 1;

	//7c0 = {r_tlb_entry_select, r_tlb_data_or_insn}
	//7c1 = {r_tlb_ppn, r_tlb_daguxwrv, r_tlb_size}
	//7c2 = {r_tlb_vpn}

	csr_write(0x7c0, ((entry & kTlbEntriesMask) << 1) | (id == 0 ? 0 : 1));
	csr_write(0x7c1, (ppn << 10) | (daguxwrv << 2) | size);
	csr_write(0x7c2, vpn);
}

#ifdef CONFIG_32BIT
static bool translate_va_pa_sv32(uint32_t satp, uintptr_t va, bool execute, bool read, bool write, uintptr_t *pPa)
{
	//sv32 implementation
	uint32_t satp_ppn = satp & kRv32SatpPpnMask;
	uint32_t satp_ppn_4k = satp_ppn << kSvCommonVaPageWidth;

	uint32_t va_vpn = (va & kSv32VaMask) >> kSvCommonVaPageWidth;

	//PTE at address satp.ppn × PAGESIZE + va.vpn[1] × PTESIZE
	uint32_t pte1_addr = satp_ppn_4k + (va_vpn >> kSv32VaVpnWidth) * kSv32PteSizeBytes;
	uint32_t pte1 = *(uint32_t *)pte1_addr;

	//if megapage
	if (pte1 & kSvCommonPteXWRMask)
	{
		//if megapage: i = 1, LEVELS = 2
		//pa.ppn[i − 1 : 0] = va.vpn[i − 1 : 0]
		//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i]

		//if valid, set A
		if (pte1 & kSvCommonPteVMask)
		{
			pte1 |= kSvCommonPteAMask;
			//and write back
			*(uint32_t *)pte1_addr = pte1;
		}

		//check valid and permissions
		bool valid = (pte1 & kSvCommonPteVMask) ? true : false;
		if (execute && ((pte1 & kSvCommonPteXMask) == 0))
			valid = false;
		if (write && ((pte1 & kSvCommonPteWMask) == 0))
			valid = false;
		if (read && ((pte1 & kSvCommonPteRMask) == 0))
			valid = false;

		*pPa = ((((pte1 >> kSv32PtePpn1Shift) << kSv32VaVpnWidth)
			| (va_vpn & kSv32VaVpnMask)) << kSvCommonVaPageWidth)
			| (va & kSvCommonVaPageMask);
		return valid;
	}

	//leaf node
	//PTE at address pte.ppn × PAGESIZE + va.vpn[0] × PTESIZE
	uint32_t pte1_ppn_4k = (pte1 >> kSv32PtePpn0Shift) << kSvCommonVaPageWidth;
	uint32_t pte0_addr = pte1_ppn_4k + (va_vpn & kSv32VaVpnMask) * kSv32PteSizeBytes;
	uint32_t pte0 = *(uint32_t *)pte0_addr;

	//check permissions
	bool valid = (pte0 & kSvCommonPteVMask) ? true : false;
	if (execute && ((pte0 & kSvCommonPteXMask) == 0))
		valid = false;
	if (write && ((pte0 & kSvCommonPteWMask) == 0))
		valid = false;
	if (read && ((pte0 & kSvCommonPteRMask) == 0))
		valid = false;

	//if valid, set A
	if (pte0 & kSvCommonPteVMask)
	{
		pte0 |= kSvCommonPteAMask;
		//and write back
		*(uint32_t *)pte0_addr = pte0;
	}

	//i = 0
	//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i].
	*pPa = ((pte0 >> kSv32PtePpn0Shift) << kSvCommonVaPageWidth) | (va & kSvCommonVaPageMask);

	return valid;
}

void handle_tlb_miss_sv32(bool data_or_insn, unsigned long tval)
{
	unsigned int entry = tlb_entry++;

	//For Sv32, PTESIZE=4 and LEVELS=2
	//Let a be satp.ppn × PAGESIZE, and let i = LEVELS − 1.
	//i = 1
	uint32_t satp = csr_read(CSR_SATP);
	uint32_t satp_ppn = satp & kRv32SatpPpnMask;
	uint32_t satp_ppn_4k = satp_ppn << kSvCommonVaPageWidth;

	uint32_t tval_vpn = (tval & kSv32VaMask) >> kSvCommonVaPageWidth;

	//PTE at address satp.ppn × PAGESIZE + va.vpn[1] × PTESIZE
	uint32_t pte1_addr = satp_ppn_4k + (tval_vpn >> kSv32VaVpnWidth) * kSv32PteSizeBytes;
	uint32_t pte1 = *(uint32_t *)pte1_addr;

	//if megapage OR pte not valid
	if ((pte1 & kSvCommonPteXWRMask) || ((pte1 & kSvCommonPteVMask) == 0))
	{
		//if not valid: write anything

		//if megapage: i = 1, LEVELS = 2
		//pa.ppn[i − 1 : 0] = va.vpn[i − 1 : 0]
		//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i]

		if (pte1 & kSvCommonPteVMask)
		{
			//if valid, set A
			pte1 |= kSvCommonPteAMask;
			//and write back
			*(uint32_t *)pte1_addr = pte1;
		}

		_m_write_tlb(data_or_insn, entry,
			tval_vpn & ~kSv32VaVpnMask,							//tval.vpn[1]
			(pte1 >> kSv32PtePpn1Shift) << kSv32VaVpnWidth,		//pte.ppn[1]
			pte1 & kSvCommonPteDAGUXWRVMask,
			kTlbMega);

		return;
	}

	//leaf node
	//PTE at address pte.ppn × PAGESIZE + va.vpn[0] × PTESIZE
	uint32_t pte1_ppn_4k = (pte1 >> kSv32PtePpn0Shift) << kSvCommonVaPageWidth;
	uint32_t pte0_addr = pte1_ppn_4k + (tval_vpn & kSv32VaVpnMask) * kSv32PteSizeBytes;
	uint32_t pte0 = *(uint32_t *)pte0_addr;

	if (pte0 & kSvCommonPteVMask)
	{
		//if valid, set A
		pte0 |= kSvCommonPteAMask;
		//and write back
		*(uint32_t *)pte0_addr = pte0;
	}

	//i = 0
	//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i].
	_m_write_tlb(data_or_insn, entry,
		tval_vpn,
		pte0 >> kSv32PtePpn0Shift,
		pte0 & kSvCommonPteDAGUXWRVMask,
		kTlbRegular);
}

void handle_tlb_miss(bool data_or_insn, unsigned long tval)
{
	handle_tlb_miss_sv32(data_or_insn, tval);
}

bool translate_va_pa(uintptr_t va, bool execute, bool read, bool write, uintptr_t *pPa)
{
	uint32_t satp = csr_read(CSR_SATP);

	//check if translation is enabled
	if ((satp >> kRv32SatpModeShift) == kSatpModeBare)
	{
		//no virtual address translation
		*pPa = va;
		return true;
	}

	//assume sv32
	return translate_va_pa_sv32(satp, va, execute, read, write, pPa);
}

#else

static bool translate_va_pa_sv39(uint64_t satp, uintptr_t va, bool execute, bool read, bool write, uintptr_t *pPa)
{
	//sv39 implementation
	uint64_t satp_ppn = satp & kRv64SatpPpnMask;
	uint64_t satp_ppn_4k = satp_ppn << kSvCommonVaPageWidth;

	uint64_t va_vpn = (va & kSv39VaMask) >> kSvCommonVaPageWidth;

	//PTE at address satp.ppn × PAGESIZE + va.vpn[2] × PTESIZE
	//i = 2
	uint64_t pte2_addr = satp_ppn_4k + (va_vpn >> (kSv39VaVpnWidth * 2)) * kSv39PteSizeBytes;
	uint64_t pte2 = *(uint64_t *)pte2_addr;

	//if gigapage
	if (pte2 & kSvCommonPteXWRMask)
	{
		//if gigapage: i = 2, LEVELS = 3
		//pa.ppn[i − 1 : 0] = va.vpn[i − 1 : 0]
		//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i]

		//if valid, set A
		if (pte2 & kSvCommonPteVMask)
		{
			pte2 |= kSvCommonPteAMask;
			//and write back
			*(uint64_t *)pte2_addr = pte2;
		}

		//check valid and permissions
		bool valid = (pte2 & kSvCommonPteVMask) ? true : false;
		if (execute && ((pte2 & kSvCommonPteXMask) == 0))
			valid = false;
		if (write && ((pte2 & kSvCommonPteWMask) == 0))
			valid = false;
		if (read && ((pte2 & kSvCommonPteRMask) == 0))
			valid = false;

		//ppn2 | vpn1 | vpn0 | offset
		*pPa = ((((pte2 >> kSv39PtePpn2Shift) << (kSv39VaVpnWidth * 2))		//ppn2
				| (va_vpn & (kSv39VaVpnMask << kSv39VaVpnWidth))			//vpn1
				| (va_vpn & kSv39VaVpnMask)) << kSvCommonVaPageWidth)		//vpn0
				| (va & kSvCommonVaPageMask);								//offset
		return valid;
	}

	//intermediate node
	//i = 1
	//PTE at address pte.ppn × PAGESIZE + va.vpn[1] × PTESIZE
	uint64_t pte2_ppn_4k = (pte2 >> kSv39PtePpn0Shift) << kSvCommonVaPageWidth;
	uint64_t pte1_addr = pte2_ppn_4k + ((va_vpn >> kSv39VaVpnWidth) & kSv39VaVpnMask) * kSv39PteSizeBytes;
	uint64_t pte1 = *(uint64_t *)pte1_addr;

	//if megapage
	if (pte1 & kSvCommonPteXWRMask)
	{
		//if megapage: i = 1, LEVELS = 3
		//pa.ppn[i − 1 : 0] = va.vpn[i − 1 : 0]
		//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i]

		//if valid, set A
		if (pte1 & kSvCommonPteVMask)
		{
			pte1 |= kSvCommonPteAMask;
			//and write back
			*(uint64_t *)pte1_addr = pte1;
		}

		//check valid and permissions
		bool valid = (pte1 & kSvCommonPteVMask) ? true : false;
		if (execute && ((pte1 & kSvCommonPteXMask) == 0))
			valid = false;
		if (write && ((pte1 & kSvCommonPteWMask) == 0))
			valid = false;
		if (read && ((pte1 & kSvCommonPteRMask) == 0))
			valid = false;

		//ppn2 | ppn1 | vpn0 | offset
		*pPa = ((((pte1 >> kSv39PtePpn1Shift) << kSv39VaVpnWidth)			//ppn2, ppn1
				| (va_vpn & kSv39VaVpnMask)) << kSvCommonVaPageWidth)		//vpn0
				| (va & kSvCommonVaPageMask);								//offset
		return valid;
	}

	//leaf node
	//PTE at address pte.ppn × PAGESIZE + va.vpn[0] × PTESIZE
	uint64_t pte1_ppn_4k = (pte1 >> kSv39PtePpn0Shift) << kSvCommonVaPageWidth;
	uint64_t pte0_addr = pte1_ppn_4k + (va_vpn & kSv39VaVpnMask) * kSv39PteSizeBytes;
	uint64_t pte0 = *(uint64_t *)pte0_addr;

	//check permissions
	bool valid = (pte0 & kSvCommonPteVMask) ? true : false;
	if (execute && ((pte0 & kSvCommonPteXMask) == 0))
		valid = false;
	if (write && ((pte0 & kSvCommonPteWMask) == 0))
		valid = false;
	if (read && ((pte0 & kSvCommonPteRMask) == 0))
		valid = false;

	//if valid, set A
	if (pte0 & kSvCommonPteVMask)
	{
		pte0 |= kSvCommonPteAMask;
		//and write back
		*(uint64_t *)pte0_addr = pte0;
	}

	//i = 0
	//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i].
	*pPa = ((pte0 >> kSv32PtePpn0Shift) << kSvCommonVaPageWidth) | (va & kSvCommonVaPageMask);

	return valid;
}

void handle_tlb_miss_sv39(bool data_or_insn, unsigned long tval)
{
	unsigned int entry = tlb_entry++;

	//LEVELS equals 3 and PTESIZE equals 8.
	//Let a be satp.ppn × PAGESIZE, and let i = LEVELS − 1.
	//i = 2
	uint64_t satp = csr_read(CSR_SATP);
	uint64_t satp_ppn = satp & kRv64SatpPpnMask;
	uint64_t satp_ppn_4k = satp_ppn << kSvCommonVaPageWidth;

	uint64_t tval_vpn = (tval & kSv39VaMask) >> kSvCommonVaPageWidth;

	//PTE at address satp.ppn × PAGESIZE + va.vpn[2] × PTESIZE
	uint64_t pte2_addr = satp_ppn_4k + (tval_vpn >> (kSv39VaVpnWidth * 2)) * kSv39PteSizeBytes;
	uint64_t pte2 = *(uint64_t *)pte2_addr;

	//if gigapage OR pte not valid
	if ((pte2 & kSvCommonPteXWRMask) || ((pte2 & kSvCommonPteVMask) == 0))
	{
		//if not valid: write anything

		//if gigapage: i = 2, LEVELS = 3
		//pa.ppn[i − 1 : 0] = va.vpn[i − 1 : 0]
		//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i]

		if (pte2 & kSvCommonPteVMask)
		{
			//if valid, set A
			pte2 |= kSvCommonPteAMask;
			//and write back
			*(uint64_t *)pte2_addr = pte2;
		}

		_m_write_tlb(data_or_insn, entry,
			tval_vpn & (kSv39VaVpnMask << (kSv39VaVpnWidth * 2)),		//{tval.vpn[2], 0, 0}
			(pte2 >> kSv39PtePpn2Shift) << (kSv39VaVpnWidth * 2),		//{pte.ppn[2], 0, 0}
			pte2 & kSvCommonPteDAGUXWRVMask,
			kTlbGiga);

		return;
	}

	//intermediate node
	//i = 1
	//PTE at address pte.ppn × PAGESIZE + va.vpn[1] × PTESIZE
	uint64_t pte2_ppn_4k = (pte2 >> kSv39PtePpn0Shift) << kSvCommonVaPageWidth;
	uint64_t pte1_addr = pte2_ppn_4k + ((tval_vpn >> kSv39VaVpnWidth) & kSv39VaVpnMask) * kSv39PteSizeBytes;
	uint64_t pte1 = *(uint64_t *)pte1_addr;

	//if megapage OR pte not valid
	if ((pte1 & kSvCommonPteXWRMask) || ((pte1 & kSvCommonPteVMask) == 0))
	{
		//if not valid: write anything

		//if megapage: i = 1, LEVELS = 3
		//pa.ppn[i − 1 : 0] = va.vpn[i − 1 : 0]
		//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i]

		if (pte1 & kSvCommonPteVMask)
		{
			//if valid, set A
			pte1 |= kSvCommonPteAMask;
			//and write back
			*(uint64_t *)pte1_addr = pte1;
		}

		_m_write_tlb(data_or_insn, entry,
			tval_vpn & ~kSv39VaVpnMask,							//{tval.vpn[2], tval.vpn[1], 0}
			(pte1 >> kSv39PtePpn1Shift) << kSv39VaVpnWidth,		//{pte.ppn[2], pte.ppn[1], 0}
			pte1 & kSvCommonPteDAGUXWRVMask,
			kTlbMega);

		return;
	}

	//leaf node
	//i = 0
	//PTE at address pte.ppn × PAGESIZE + va.vpn[0] × PTESIZE
	uint64_t pte1_ppn_4k = (pte1 >> kSv39PtePpn0Shift) << kSvCommonVaPageWidth;
	uint64_t pte0_addr = pte1_ppn_4k + (tval_vpn & kSv39VaVpnMask) * kSv39PteSizeBytes;
	uint64_t pte0 = *(uint64_t *)pte0_addr;

	if (pte0 & kSvCommonPteVMask)
	{
		//if valid, set A
		pte0 |= kSvCommonPteAMask;
		//and write back
		*(uint64_t *)pte0_addr = pte0;
	}

	//pa.ppn[LEVELS − 1 : i] = pte.ppn[LEVELS − 1 : i].
	_m_write_tlb(data_or_insn, entry,
		tval_vpn,								//{tval.vpn[2], tval.vpn[1], tval.vpn[0]}
		pte0 >> kSv39PtePpn0Shift,				//{pte.ppn[2], pte.ppn[1], pte.ppn[0]}
		pte0 & kSvCommonPteDAGUXWRVMask,
		kTlbRegular);
}

void handle_tlb_miss(bool data_or_insn, unsigned long tval)
{
	handle_tlb_miss_sv39(data_or_insn, tval);
}

bool translate_va_pa(uintptr_t va, bool execute, bool read, bool write, uintptr_t *pPa)
{
	uint64_t satp = csr_read(CSR_SATP);

	//check if translation is enabled
	if ((satp >> kRv64SatpModeShift) == kSatpModeBare)
	{
		//no virtual address translation
		*pPa = va;
		return true;
	}

	//assume sv39
	return translate_va_pa_sv39(satp, va, execute, read, write, pPa);
}
#endif

unsigned long get_reg(unsigned long *pRegs, uint32_t id)
{
	if (id == 0)
		return 0;
	else if (id == 4)
		return csr_read(CSR_MSCRATCH);
	else
		return pRegs[id];
}

#ifdef CONFIG_64BIT
static void set_reg64(unsigned long *pRegs, uint32_t id, uint64_t val)
{
	if (id == 0)
	{
		//do nothing
	}
	else if (id == 4)
		csr_write(CSR_MSCRATCH, val);
	else
		pRegs[id] = val;
}
#endif

static void set_reg32(unsigned long *pRegs, uint32_t id, uint32_t _val)
{
	unsigned long val = _val;

#ifdef CONFIG_64BIT
	//sign extend
	if (_val & (1 << 31))
		val |= 0xffffffff00000000ull;
#endif
	if (id == 0)
	{
		//do nothing
	}
	else if (id == 4)
		csr_write(CSR_MSCRATCH, val);
	else
		pRegs[id] = val;
}

#ifdef CONFIG_64BIT
static void set_reg_full(unsigned long *pRegs, uint32_t id, unsigned long val)
{
	set_reg64(pRegs, id, val);
}
#else
static void set_reg_full(unsigned long *pRegs, uint32_t id, unsigned long val)
{
	set_reg32(pRegs, id, val);
}
#endif


long _m_divdi3(long a, long b);
unsigned long _m_udivdi3(unsigned long a, unsigned long b);
long _m_umoddi3(long a, long b);
unsigned long _m_moddi3(unsigned long a, unsigned long b);

int _m_divsi3(int a, int b);
unsigned int _m_udivsi3(unsigned int a, unsigned int b);
int _m_umodsi3(int a, int b);
unsigned int _m_modsi3(unsigned int a, unsigned int b);

static unsigned long amo_add(unsigned long original, unsigned long incoming)
{
	return original + incoming;
}

static unsigned long amo_and(unsigned long original, unsigned long incoming)
{
	return original & incoming;
}

static unsigned long amo_swap(unsigned long original, unsigned long incoming)
{
	return incoming;
}

static unsigned long amo_or(unsigned long original, unsigned long incoming)
{
	return original | incoming;
}

static unsigned long amo_xor(unsigned long original, unsigned long incoming)
{
	return original ^ incoming;
}

static bool amo_op(unsigned long *pRegs, unsigned int rd, unsigned int rs1, unsigned int rs2,
	unsigned long (*op)(unsigned long, unsigned long), unsigned int size)
{
	uintptr_t pa;
	//read and write
	bool success = translate_va_pa(get_reg(pRegs, rs1), false, true, true, &pa);

	if (success)
	{
		if (size == 2)		//32-bit
		{
			/* when run on 32-bit
			load a 32-bit value, op takes ulong (32-bit), get_reg returns 32-bit
			result is 32-bit
			set_reg32 sets a 32-bit value

			when run on 64-bit
			load a 32-bit value (zero-extend to 64-bit), op takes 64-bit, get_reg returns 64-bit
			result is 64-bit, but truncated to 32-bit
			set_reg32 sign-extends the truncated 32-bit result to 64-bit
			*/
			uint32_t original_value = *(uint32_t *)pa;
			uint32_t new_value = op(original_value, get_reg(pRegs, rs2));
			set_reg32(pRegs, rd, original_value);
			*(uint32_t *)pa = new_value;
		}
		//rv64-only encoding
#ifdef CONFIG_64BIT
		else				//64-bit
		{
			uint64_t original_value = *(uint64_t *)pa;
			uint64_t new_value = op(original_value, get_reg(pRegs, rs2));
			set_reg_full(pRegs, rd, original_value);
			*(uint64_t *)pa = new_value;
		}
#endif

		return true;
	}
	else
	{
		//store page fault at va
		csr_write(CSR_MTVAL, get_reg(pRegs, rs1));
		csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapStorePageFault);

		//now fall through out of TrapIllegalIns
		return false;
	}
}

static void delegate_to_super(void)
{
	//delegated to supervisor mode
	//manually perform a trap with the help of mret
	unsigned long mstatus = csr_read(CSR_MSTATUS);
	unsigned long mstatus_mpp = (mstatus >> 11) & 3;	//previous mode before machine mode trap
	unsigned long mstatus_sie = (mstatus >> 1) & 1;		//current sie

	// spie <= sie
	mstatus = mstatus & ~(1 << 5);					//clear spie
	mstatus = mstatus | (mstatus_sie << 5);			//set spie

	// sie <= 0
	mstatus = mstatus & ~(1 << 1);					//clear sie

	// spp <= previous mode
	mstatus = mstatus & ~(1 << 8);					//clear spp
	mstatus = mstatus | ((mstatus_mpp & 1) << 8);	//set spp based on previous mode

	mstatus = mstatus & ~(3 << 11);					//clear mpp
	mstatus = mstatus | (1 << 11);					//set supervisor mode to return to

	csr_write(CSR_MSTATUS, mstatus);

	// sepc <= pc
	csr_write(CSR_SEPC, csr_read(CSR_MEPC));

	// pc <= stvec
	csr_write(CSR_MEPC, csr_read(CSR_STVEC));		//the address we want to return to

	// stval <= failing access VA / zero
	csr_write(CSR_STVAL, csr_read(CSR_MTVAL));

	// scause <= {interrupt ? 1 :0, code}
	csr_write(CSR_SCAUSE, csr_read(CSR_MCAUSE));
}

asmlinkage void _m_exception_c(unsigned long *pRegs)
{
	unsigned long cause = csr_read(CSR_MCAUSE);
	unsigned long tval = csr_read(CSR_MTVAL);

	if ((cause & kMcauseInterruptMask) == kMcauseNotInterrupt)
	{
		//exception
		switch (cause & 31)
		{
			case TrapIllegalIns:
			{
				bool fall_through = true;

				unsigned int opcode = tval & 127;
				switch (opcode)
				{
					//system
					case 0b1110011:
					{
						unsigned int rd = (tval >> 7) & 31;
						unsigned int funct3 = (tval >> 12) & 7;
						unsigned int csr = tval >> 20;

						unsigned long rd_value = 0;

						//csrrs
						if (funct3 == 0b010)
						{
							ILL_INC(ill_system_csrrs);

							switch (csr)
							{
								//time
								case 0xC01:
								{
									volatile unsigned int *pTime = (volatile unsigned int *)0x1000600;
									rd_value = *pTime;
									fall_through = false;
									break;
								}
#ifdef CONFIG_32BIT
								//timer is only 32 bits wide
								case 0xC81:
									rd_value = 0;
									fall_through = false;
									break;
#endif
								default:
									break;
							}
						}

						if (!fall_through)
							set_reg_full(pRegs, rd, rd_value);
						break;
					}
#ifdef CONFIG_64BIT
					//op-32
					case 0b0111011:
					{
						unsigned int rd = (tval >> 7) & 31;
						unsigned int funct3 = (tval >> 12) & 7;
						unsigned int rs1 = (tval >> 15) & 31;
						unsigned int rs2 = (tval >> 20) & 31;
						unsigned int funct7 = tval >> 25;

						switch (funct7)
						{
							//muldiv
							case 0b0000001:
							{
								uint32_t rs1_32 = get_reg(pRegs, rs1) & 0xffffffff;
								uint32_t rs2_32 = get_reg(pRegs, rs2) & 0xffffffff;

								int s_rs1 = rs1_32;
								int s_rs2 = rs2_32;

								unsigned int u_rs1 = rs1_32;
								unsigned int u_rs2 = rs2_32;

								//outputs are sign-extended to 64-bit

								switch (funct3)
								{
									//divw
									case 0b100:
									{
										ILL_INC(ill_op32_divw);
										set_reg32(pRegs, rd, _m_divdi3(s_rs1, s_rs2));
										fall_through = false;
										break;
									}
									//divuw
									case 0b101:
									{
										ILL_INC(ill_op32_divuw);
										set_reg32(pRegs, rd, _m_udivdi3(u_rs1, u_rs2));
										fall_through = false;
										break;
									}
									//remw
									case 0b110:
									{
										ILL_INC(ill_op32_remw);
										set_reg32(pRegs, rd, _m_moddi3(s_rs1, s_rs2));
										fall_through = false;
										break;
									}
									//remuw
									case 0b111:
									{
										ILL_INC(ill_op32_remuw);
										set_reg32(pRegs, rd, _m_umoddi3(u_rs1, u_rs2));
										fall_through = false;
										break;
									}
									default:
										break;	//did not match the instruction
								}
							}
							default:
								break;		//did not decode this major class
						}
						break;				//break from op
					}
#endif
					//op
					case 0b0110011:
					{
						unsigned int rd = (tval >> 7) & 31;
						unsigned int funct3 = (tval >> 12) & 7;
						unsigned int rs1 = (tval >> 15) & 31;
						unsigned int rs2 = (tval >> 20) & 31;
						unsigned int funct7 = tval >> 25;

						switch (funct7)
						{
							//muldiv
							case 0b0000001:
							{
								switch (funct3)
								{
									//div
									case 0b100:
									{
										ILL_INC(ill_op_div);
										set_reg_full(pRegs, rd, _m_divdi3(get_reg(pRegs, rs1), get_reg(pRegs, rs2)));
										fall_through = false;
										break;
									}
									//divu
									case 0b101:
									{
										ILL_INC(ill_op_divu);
										set_reg_full(pRegs, rd, _m_udivdi3(get_reg(pRegs, rs1), get_reg(pRegs, rs2)));
										fall_through = false;
										break;
									}
									//rem
									case 0b110:
									{
										ILL_INC(ill_op_rem);
										set_reg_full(pRegs, rd, _m_moddi3(get_reg(pRegs, rs1), get_reg(pRegs, rs2)));
										fall_through = false;
										break;
									}
									//remu
									case 0b111:
									{
										ILL_INC(ill_op_remu);
										set_reg_full(pRegs, rd, _m_umoddi3(get_reg(pRegs, rs1), get_reg(pRegs, rs2)));
										fall_through = false;
										break;
									}
									default:
										break;	//did not match the instruction
								}
							}
							default:
								break;		//did not decode this major class
						}
						break;				//break from op
					}
					//amo
					case 0b0101111:
					{
						unsigned int rd = (tval >> 7) & 31;
						unsigned int funct3 = (tval >> 12) & 3;
						unsigned int rs1 = (tval >> 15) & 31;
						unsigned int rs2 = (tval >> 20) & 31;
						unsigned int funct5 = tval >> 27;

						//These AMO instructions atomically load a data value from the address in rs1,
						//place the value into register rd, apply a binary operator to the loaded value
						//and the original value in rs2, then store the result back to the address in rs1.	
						switch (funct5)
						{
							//AMOSWAP.W/D
							case 0b00001:
							{
								ILL_INC(ill_amo_amoswap);
								if (amo_op(pRegs, rd, rs1, rs2, &amo_swap, funct3))
									fall_through = false;

								break;		//break from this instruction
							}
							//AMOADD.W/D
							case 0b00000:
							{
								ILL_INC(ill_amo_amoadd);
								if (amo_op(pRegs, rd, rs1, rs2, &amo_add, funct3))
									fall_through = false;

								break;		//break from this instruction
							}
							//AMOXOR.W/D
							case 0b00100:
							{
								ILL_INC(ill_amo_amoxor);
								if (amo_op(pRegs, rd, rs1, rs2, &amo_xor, funct3))
									fall_through = false;

								break;		//break from this instruction
							}
							//AMOAND.W/D
							case 0b01100:
							{
								ILL_INC(ill_amo_amoand);
								if (amo_op(pRegs, rd, rs1, rs2, &amo_and, funct3))
									fall_through = false;

								break;		//break from this instruction
							}
							//AMOOR.W/D
							case 0b01000:
							{
								ILL_INC(ill_amo_amoor);
								if (amo_op(pRegs, rd, rs1, rs2, &amo_or, funct3))
									fall_through = false;

								break;		//break from this instruction
							}
							//LR.W/D
							case 0b00010:
							{
								ILL_INC(ill_amo_lr);

								uintptr_t pa;
								//read only
								bool success = translate_va_pa(get_reg(pRegs, rs1), false, true, false, &pa);

								if (success)
								{
									if (funct3 == 2)		//32-bit
									{
										/* on 32-bit
										load the 32-bit value
										set_reg32 takes 32-bit input

										on 64-bit
										load the 32-bit value
										set_reg32 takes 32-bit input
										sign-extends to 64-bit
										*/
										set_reg32(pRegs, rd, *(uint32_t *)pa);
									}
#ifdef CONFIG_64BIT
									else					//64-bit
									{
										set_reg_full(pRegs, rd, *(uint64_t *)pa);
									}
#endif
									reservation_addr_pa = pa;

									fall_through = false;
								}
								else
								{
									//load page fault at va
									csr_write(CSR_MTVAL, get_reg(pRegs, rs1));
									csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapLoadPageFault);

									//now fall through out of TrapIllegalIns
								}
								break;		//break from this instruction
							}
							//SC.W/D
							case 0b00011:
							{
								ILL_INC(ill_amo_sc);

								uintptr_t pa;
								//write only
								bool success = translate_va_pa(get_reg(pRegs, rs1), false, false, true, &pa);

								if (success)
								{
									//SC.W conditionally writes a word in rs2 to the address in rs1:
									//the SC.W succeeds only if the reservation is still valid and
									//the reservation set contains the bytes being written.
									//If the SC.W succeeds, the instruction writes the word in rs2
									//to memory, and it writes zero to rd. If the SC.W fails,
									//the instruction does not write to memory, and it writes a
									//nonzero value to rd.

									if (pa == reservation_addr_pa)
									{
										//success
										if (funct3 == 2)		//32-bit
										{
											*(uint32_t *)pa = get_reg(pRegs, rs2);
										}
#ifdef CONFIG_64BIT
										else					//64-bit
										{
											*(uint64_t *)pa = get_reg(pRegs, rs2);
										}
#endif

										set_reg_full(pRegs, rd, 0);
									}
									else	//failure
										set_reg_full(pRegs, rd, 1);

									//token invalidation
									reservation_addr_pa = -1;

									fall_through = false;
								}
								else
								{
									//store page fault at va
									csr_write(CSR_MTVAL, get_reg(pRegs, rs1));
									csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapStorePageFault);

									//now fall through out of TrapIllegalIns
								}
								break;		//break from this instruction
							}
							default:
								break;		//did not decode this amo instruction
						}
						break;				//break from amo
					}
					default:
						break;				//did not decode this class
				}

				if (!fall_through)
				{
					//move to next instruction
					csr_write(CSR_MEPC, csr_read(CSR_MEPC) + 4);
					break;
				}
			} /* fall through */
			case TrapInsAddrMisaligned:
			case TrapInsAccFault:
			case TrapBreakpoint:
			case TrapLoadFault:
			case TrapStoreFault:
			case TrapUserCall:
			case TrapInsPageFault:
			case TrapLoadPageFault:
			case TrapStorePageFault:
			default:
			{
				delegate_to_super();
				break;
			}

			case TrapLoadMisaligned:
			case TrapStoreMisaligned:
			{
				bool fall_through = true;
				// ft245_put_string("trap misaligned epc ");
				unsigned long inst_va = csr_read(CSR_MEPC);
				unsigned long inst_pa;

				// ft245_put_hex_num(inst_va);
				// ft245_put_string(" ");

				//not sure this can fail
				translate_va_pa(inst_va, true, false, false, &inst_pa);

				// ft245_put_hex_num(inst_pa);
				// ft245_put_string(" inst ");

				union {
					unsigned int inst;
					unsigned char b[4];
				} i;

				i.b[0] = ((unsigned char *)inst_pa)[0];
				i.b[1] = ((unsigned char *)inst_pa)[1];
				i.b[2] = ((unsigned char *)inst_pa)[2];
				i.b[3] = ((unsigned char *)inst_pa)[3];

				unsigned int inst = i.inst;

				// ft245_put_hex_num(inst);

				unsigned int opcode = inst & 127;
				switch (opcode)
				{
					//load
					case 0b0000011:
					{
						unsigned int rd = (inst >> 7) & 31;
						unsigned int funct3 = (inst >> 12) & 7;
						unsigned int rs1 = (inst >> 15) & 31;
						unsigned int imm = inst >> 20;

						union {
#ifdef CONFIG_64BIT
							unsigned char rd_value_c[8];
#elif CONFIG_32BIT
							unsigned char rd_value_c[4];
#endif
							unsigned long rd_value_i;
						} u;
						unsigned long va_lo = imm;
						unsigned long va_hi;

						uintptr_t pa_lo, pa_hi;
						bool success;
						unsigned long count;

						// ft245_put_string(" load va ");

						//sign-extend the immediate
						if (inst >> 31)
							va_lo |= ((unsigned long)-1) & ~0xffful;
						
						//compute the full address
						//todo we have tval which is epc
						va_lo += get_reg(pRegs, rs1);

						// ft245_put_hex_num(va_lo);

						//compute last byte
						va_hi = va_lo + (1 << funct3) - 1;

						// ft245_put_string(" ");
						// ft245_put_hex_num(va_hi);

						//read lo
						success = translate_va_pa(va_lo, false, true, false, &pa_lo);

						// if (success)
						// 	ft245_put_string(" success");
						// else
						// 	ft245_put_string(" failure");

						// ft245_put_string(" load pa lo ");
						// ft245_put_hex_num(pa_lo);

						if (!success)
						{
							//load page fault at va_lo
							csr_write(CSR_MTVAL, va_lo);
							csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapLoadPageFault);
							// ft245_put_string("\n");
							break;
						}

						//read hi
						success = translate_va_pa(va_hi, false, true, false, &pa_hi);

						// if (success)
						// 	ft245_put_string(" success");
						// else
						// 	ft245_put_string(" failure");

						// ft245_put_string(" load pa hi ");
						// ft245_put_hex_num(pa_hi);

						if (!success)
						{
							//load page fault at va_hi
							csr_write(CSR_MTVAL, va_hi);
							csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapLoadPageFault);
							// ft245_put_string("\n");
							break;
						}

						fall_through = false;
						u.rd_value_i = 0;

						//todo optimise
						for (count = 0; count < (1 << funct3); count++)
						{
							uintptr_t pa;
							translate_va_pa(va_lo + count, false, true, false, &pa);

							u.rd_value_c[count] = *(unsigned char *)pa;
						}

						// ft245_put_string(" reading ");
						// ft245_put_hex_num(u.rd_value_i);

						// ft245_put_string("\n");

						set_reg_full(pRegs, rd, u.rd_value_i);
						break;
					}
					//store
					case 0b0100011:
					{
						unsigned int funct3 = (inst >> 12) & 7;
						unsigned int rs1 = (inst >> 15) & 31;
						unsigned int rs2 = (inst >> 20) & 31;
						unsigned int imm = ((inst >> 7) & 31) | (inst >> 25);

						union {
#ifdef CONFIG_64BIT
							unsigned char rd_value_c[8];
#elif CONFIG_32BIT
							unsigned char rd_value_c[4];
#endif
							unsigned long rd_value_i;
						} u;
						unsigned long va_lo = imm;
						unsigned long va_hi;

						uintptr_t pa_lo, pa_hi;
						bool success;
						unsigned long count;

						// ft245_put_string(" store va ");

						//sign-extend the immediate
						if (inst >> 31)
							va_lo |= ((unsigned long)-1) & ~0xffful;
						
						//compute the full address
						va_lo += get_reg(pRegs, rs1);

						// ft245_put_hex_num(va_lo);

						//compute last byte
						va_hi = va_lo + (1 << funct3) -1;

						// ft245_put_string(" ");
						// ft245_put_hex_num(va_hi);

						//write lo
						success = translate_va_pa(va_lo, false, false, true, &pa_lo);

						// if (success)
						// 	ft245_put_string(" success");
						// else
						// 	ft245_put_string(" failure");

						// ft245_put_string(" store pa lo ");
						// ft245_put_hex_num(pa_lo);

						if (!success)
						{
							//store page fault at va_lo
							csr_write(CSR_MTVAL, va_lo);
							csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapStorePageFault);
							// ft245_put_string("\n");
							break;
						}

						//write hi
						success = translate_va_pa(va_hi, false, false, true, &pa_hi);

						// if (success)
						// 	ft245_put_string(" success");
						// else
						// 	ft245_put_string(" failure");

						// ft245_put_string(" store pa hi ");
						// ft245_put_hex_num(pa_hi);

						if (!success)
						{
							//store page fault at va_hi
							csr_write(CSR_MTVAL, va_hi);
							csr_write(CSR_MCAUSE, kMcauseNotInterrupt | TrapStorePageFault);
							// ft245_put_string("\n");
							break;
						}

						fall_through = false;
						u.rd_value_i = get_reg(pRegs, rs2);

						// ft245_put_string(" writing ");
						// ft245_put_hex_num(u.rd_value_i);

						// ft245_put_string("\n");

						//todo optimise
						for (count = 0; count < (1 << funct3); count++)
						{
							uintptr_t pa;
							translate_va_pa(va_lo + count, false, true, false, &pa);

							*(unsigned char *)pa = u.rd_value_c[count];
						}

						break;
					}
					default:
						//decode failure
						break;
				}

				if (!fall_through)
				{
					//move to next instruction
					csr_write(CSR_MEPC, csr_read(CSR_MEPC) + 4);
					break;
				}
				else
				{
					delegate_to_super();
					break;
				}
			}

			//call from supervisor code
			case TrapSuperCall:
			{
				ecall++;

#ifdef ILL_COUNT
				if ((ecall & 127) == 0)
				{
					ft245_put_string("\nILLEGAL INS DUMP");
					ft245_put_string("\nsystem_csrrs "); ft245_put_hex_num(ill_system_csrrs);
					ft245_put_string("\nop32_divw "); ft245_put_hex_num(ill_op32_divw);
					ft245_put_string("\nop32_divuw "); ft245_put_hex_num(ill_op32_divuw);
					ft245_put_string("\nop32_remw "); ft245_put_hex_num(ill_op32_remw);
					ft245_put_string("\nop32_remuw "); ft245_put_hex_num(ill_op32_remuw);
					ft245_put_string("\nop_div "); ft245_put_hex_num(ill_op_div);
					ft245_put_string("\nop_divu "); ft245_put_hex_num(ill_op_divu);
					ft245_put_string("\nop_rem "); ft245_put_hex_num(ill_op_rem);
					ft245_put_string("\nop_remu "); ft245_put_hex_num(ill_op_remu);
					ft245_put_string("\namo_amoswap "); ft245_put_hex_num(ill_amo_amoswap);
					ft245_put_string("\namo_amoadd "); ft245_put_hex_num(ill_amo_amoadd);
					ft245_put_string("\namo_amoxor "); ft245_put_hex_num(ill_amo_amoxor);
					ft245_put_string("\namo_amoand "); ft245_put_hex_num(ill_amo_amoand);
					ft245_put_string("\namo_amoor "); ft245_put_hex_num(ill_amo_amoor);
					ft245_put_string("\namo_lr "); ft245_put_hex_num(ill_amo_lr);
					ft245_put_string("\namo_sc "); ft245_put_hex_num(ill_amo_sc);
				}
#endif

				//eid in a7
				switch (get_reg(pRegs, 17) & 0xffffffff)
				{
					//base extension
					case 0x10:
					{
						//fid in a6
						switch (get_reg(pRegs, 16) & 0xffffffff)
						{
							//Function: Get SBI specification version (FID #0)
							case 0x0:
							{
								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, (0 << 24) | 2);
								break;
							}
							//Function: Get SBI implementation ID (FID #1)
							case 0x1:
							{
								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, 0xbeefcafe);
								break;
							}
							//Function: Get SBI implementation version (FID #2)
							case 0x2:
							{
								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, 1);
								break;
							}
							//Function: Probe SBI extension (FID #3)
							case 0x3:
							{
								//extension not supported
								unsigned int support = 0;
								
								//a0
								switch (get_reg(pRegs, 10) & 0xffffffff)
								{
									//TIME
									case 0x54494D45:
										support = 1;
										break;
									default:
										break;
								}

								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, support);
								break;
							}
							//todo replace these with csr reads
							//though in retrospect why bother
							//Function: Get machine vendor ID (FID #4)
							case 0x4:
							{
								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, 0);		//non-commercial implementation
								break;
							}
							//Function: Get machine architecture ID (FID #5)
							case 0x5:
							{
								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, 0);		//non-commercial implementation
								break;
							}
							//Function: Get machine implementation ID (FID #6)
							case 0x6:
							{
								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								set_reg_full(pRegs, 11, 0);
								break;
							}
							default:
							{
								set_reg_full(pRegs, 10, -2);	//SBI_ERR_NOT_SUPPORTED
								break;
							}
						}
						break;
					}
					//Timer Extension (EID #0x54494D45 "TIME")
					case 0x54494D45:
					{
						//fid in a6
						switch (get_reg(pRegs, 16) & 0xffffffff)
						{
							//struct sbiret sbi_set_timer(uint64_t stime_value)
							case 0:
							{
								uint64_t stime_value = 0;
#ifdef CONFIG_64BIT
								stime_value = get_reg(pRegs, 10);
#elif CONFIG_32BIT
								stime_value = ((uint64_t)get_reg(pRegs, 11) << 32) | get_reg(pRegs, 10);
#endif
								//write the low 32 bits of the value
								volatile unsigned int *pTime = (volatile unsigned int *)0x1000600;
								*pTime = stime_value;

								csr_set(CSR_MIE, 1 << 7);		//mtie

								set_reg_full(pRegs, 10, 0);		//SBI_SUCCESS
								break;
							}
							default:
								set_reg_full(pRegs, 10, -2);		//SBI_ERR_NOT_SUPPORTED
								break;
						}
						break;
					}
					default:
					{
						set_reg_full(pRegs, 10, -2);		//SBI_ERR_NOT_SUPPORTED
						break;
					}
				}

				//move to next instruction
				csr_write(CSR_MEPC, csr_read(CSR_MEPC) + 4);
				break;
			}

			//call from machine code
			case TrapMachineCall:
				//do something
				break;

			case TrapInstTlbMiss:
			{
				handle_tlb_miss(false, tval);
				break;
			}
			case TrapDataTlbMiss:
			{
				handle_tlb_miss(true, tval);
				break;
			}
		}
	}
	else
	{
		//interrupt
		switch (cause & 31)
		{
			case TrapMTimerInt:
			{
				volatile unsigned int *pTime = (volatile unsigned int *)0x1000600;
				//clear timecmp to unset mtip
				*pTime = -1;

				//set STIP
				csr_set(CSR_MIP, 1 << 5);
				break;
			}
			case TrapSTimerInt:
			{
				//clear STIP
				csr_clear(CSR_MIP, 1 << 5);

				delegate_to_super();
				break;
			}
			default:
				break;
		}
	}
}

asmlinkage void __init setup_vm(uintptr_t dtb_pa)
{
	uintptr_t va, pa, end_va;
	uintptr_t load_pa = (uintptr_t)(&_start);
	uintptr_t load_sz = (uintptr_t)(&_end) - load_pa;
	uintptr_t map_size;
#ifndef __PAGETABLE_PMD_FOLDED
	pmd_t fix_bmap_spmd, fix_bmap_epmd;
#endif

	va_pa_offset = PAGE_OFFSET - load_pa;
	pfn_base = PFN_DOWN(load_pa);

	/*
	 * Enforce boot alignment requirements of RV32 and
	 * RV64 by only allowing PMD or PGD mappings.
	 */
	map_size = PMD_SIZE;

	/* Sanity check alignment and size */
	BUG_ON((PAGE_OFFSET % PGDIR_SIZE) != 0);
	BUG_ON((load_pa % map_size) != 0);

	pt_ops.alloc_pte = alloc_pte_early;
	pt_ops.get_pte_virt = get_pte_virt_early;
#ifndef __PAGETABLE_PMD_FOLDED
	pt_ops.alloc_pmd = alloc_pmd_early;
	pt_ops.get_pmd_virt = get_pmd_virt_early;
#endif
	/* Setup early PGD for fixmap */
	create_pgd_mapping(early_pg_dir, FIXADDR_START,
			   (uintptr_t)fixmap_pgd_next, PGDIR_SIZE, PAGE_TABLE);

#ifndef __PAGETABLE_PMD_FOLDED
	/* Setup fixmap PMD */
	create_pmd_mapping(fixmap_pmd, FIXADDR_START,
			   (uintptr_t)fixmap_pte, PMD_SIZE, PAGE_TABLE);
	/* Setup trampoline PGD and PMD */
	create_pgd_mapping(trampoline_pg_dir, PAGE_OFFSET,
			   (uintptr_t)trampoline_pmd, PGDIR_SIZE, PAGE_TABLE);
	create_pmd_mapping(trampoline_pmd, PAGE_OFFSET,
			   load_pa, PMD_SIZE, PAGE_KERNEL_EXEC);
#else
	/* Setup trampoline PGD */
	create_pgd_mapping(trampoline_pg_dir, PAGE_OFFSET,
			   load_pa, PGDIR_SIZE, PAGE_KERNEL_EXEC);
#endif

	/*
	 * Setup early PGD covering entire kernel which will allows
	 * us to reach paging_init(). We map all memory banks later
	 * in setup_vm_final() below.
	 */
	end_va = PAGE_OFFSET + load_sz;
	for (va = PAGE_OFFSET; va < end_va; va += map_size)
		create_pgd_mapping(early_pg_dir, va,
				   load_pa + (va - PAGE_OFFSET),
				   map_size, PAGE_KERNEL_EXEC);

#ifndef __PAGETABLE_PMD_FOLDED
	/* Setup early PMD for DTB */
	create_pgd_mapping(early_pg_dir, DTB_EARLY_BASE_VA,
			   (uintptr_t)early_dtb_pmd, PGDIR_SIZE, PAGE_TABLE);
#ifndef CONFIG_BUILTIN_DTB
	/* Create two consecutive PMD mappings for FDT early scan */
	pa = dtb_pa & ~(PMD_SIZE - 1);
	create_pmd_mapping(early_dtb_pmd, DTB_EARLY_BASE_VA,
			   pa, PMD_SIZE, PAGE_KERNEL);
	create_pmd_mapping(early_dtb_pmd, DTB_EARLY_BASE_VA + PMD_SIZE,
			   pa + PMD_SIZE, PMD_SIZE, PAGE_KERNEL);
	dtb_early_va = (void *)DTB_EARLY_BASE_VA + (dtb_pa & (PMD_SIZE - 1));
#else /* CONFIG_BUILTIN_DTB */
	dtb_early_va = __va(dtb_pa);
#endif /* CONFIG_BUILTIN_DTB */
#else
#ifndef CONFIG_BUILTIN_DTB
	/* Create two consecutive PGD mappings for FDT early scan */
	pa = dtb_pa & ~(PGDIR_SIZE - 1);
	create_pgd_mapping(early_pg_dir, DTB_EARLY_BASE_VA,
			   pa, PGDIR_SIZE, PAGE_KERNEL);
	create_pgd_mapping(early_pg_dir, DTB_EARLY_BASE_VA + PGDIR_SIZE,
			   pa + PGDIR_SIZE, PGDIR_SIZE, PAGE_KERNEL);
	dtb_early_va = (void *)DTB_EARLY_BASE_VA + (dtb_pa & (PGDIR_SIZE - 1));
#else /* CONFIG_BUILTIN_DTB */
	dtb_early_va = __va(dtb_pa);
#endif /* CONFIG_BUILTIN_DTB */
#endif
	dtb_early_pa = dtb_pa;

	/*
	 * Bootime fixmap only can handle PMD_SIZE mapping. Thus, boot-ioremap
	 * range can not span multiple pmds.
	 */
	BUILD_BUG_ON((__fix_to_virt(FIX_BTMAP_BEGIN) >> PMD_SHIFT)
		     != (__fix_to_virt(FIX_BTMAP_END) >> PMD_SHIFT));

#ifndef __PAGETABLE_PMD_FOLDED
	/*
	 * Early ioremap fixmap is already created as it lies within first 2MB
	 * of fixmap region. We always map PMD_SIZE. Thus, both FIX_BTMAP_END
	 * FIX_BTMAP_BEGIN should lie in the same pmd. Verify that and warn
	 * the user if not.
	 */
	fix_bmap_spmd = fixmap_pmd[pmd_index(__fix_to_virt(FIX_BTMAP_BEGIN))];
	fix_bmap_epmd = fixmap_pmd[pmd_index(__fix_to_virt(FIX_BTMAP_END))];
	if (pmd_val(fix_bmap_spmd) != pmd_val(fix_bmap_epmd)) {
		WARN_ON(1);
		pr_warn("fixmap btmap start [%08lx] != end [%08lx]\n",
			pmd_val(fix_bmap_spmd), pmd_val(fix_bmap_epmd));
		pr_warn("fix_to_virt(FIX_BTMAP_BEGIN): %08lx\n",
			fix_to_virt(FIX_BTMAP_BEGIN));
		pr_warn("fix_to_virt(FIX_BTMAP_END):   %08lx\n",
			fix_to_virt(FIX_BTMAP_END));

		pr_warn("FIX_BTMAP_END:       %d\n", FIX_BTMAP_END);
		pr_warn("FIX_BTMAP_BEGIN:     %d\n", FIX_BTMAP_BEGIN);
	}
#endif
}

static void __init setup_vm_final(void)
{
	uintptr_t va, map_size;
	phys_addr_t pa, start, end;
	u64 i;

	/**
	 * MMU is enabled at this point. But page table setup is not complete yet.
	 * fixmap page table alloc functions should be used at this point
	 */
	pt_ops.alloc_pte = alloc_pte_fixmap;
	pt_ops.get_pte_virt = get_pte_virt_fixmap;
#ifndef __PAGETABLE_PMD_FOLDED
	pt_ops.alloc_pmd = alloc_pmd_fixmap;
	pt_ops.get_pmd_virt = get_pmd_virt_fixmap;
#endif
	/* Setup swapper PGD for fixmap */
	create_pgd_mapping(swapper_pg_dir, FIXADDR_START,
			   __pa_symbol(fixmap_pgd_next),
			   PGDIR_SIZE, PAGE_TABLE);

	/* Map all memory banks */
	for_each_mem_range(i, &start, &end) {
		if (start >= end)
			break;
		if (start <= __pa(PAGE_OFFSET) &&
		    __pa(PAGE_OFFSET) < end)
			start = __pa(PAGE_OFFSET);

		map_size = best_map_size(start, end - start);
		for (pa = start; pa < end; pa += map_size) {
			va = (uintptr_t)__va(pa);
			create_pgd_mapping(swapper_pg_dir, va, pa,
					   map_size, PAGE_KERNEL_EXEC);
		}
	}

	/* Clear fixmap PTE and PMD mappings */
	clear_fixmap(FIX_PTE);
	clear_fixmap(FIX_PMD);

	/* Move to swapper page table */
	csr_write(CSR_SATP, PFN_DOWN(__pa_symbol(swapper_pg_dir)) | SATP_MODE);
	local_flush_tlb_all();

	/* generic page allocation functions must be used to setup page table */
	pt_ops.alloc_pte = alloc_pte_late;
	pt_ops.get_pte_virt = get_pte_virt_late;
#ifndef __PAGETABLE_PMD_FOLDED
	pt_ops.alloc_pmd = alloc_pmd_late;
	pt_ops.get_pmd_virt = get_pmd_virt_late;
#endif
}
#else
asmlinkage void __init setup_vm(uintptr_t dtb_pa)
{
	dtb_early_va = (void *)dtb_pa;
	dtb_early_pa = dtb_pa;
}

static inline void setup_vm_final(void)
{
}
#endif /* CONFIG_MMU */

#ifdef CONFIG_STRICT_KERNEL_RWX
void protect_kernel_text_data(void)
{
	unsigned long text_start = (unsigned long)_start;
	unsigned long init_text_start = (unsigned long)__init_text_begin;
	unsigned long init_data_start = (unsigned long)__init_data_begin;
	unsigned long rodata_start = (unsigned long)__start_rodata;
	unsigned long data_start = (unsigned long)_data;
	unsigned long max_low = (unsigned long)(__va(PFN_PHYS(max_low_pfn)));

	set_memory_ro(text_start, (init_text_start - text_start) >> PAGE_SHIFT);
	set_memory_ro(init_text_start, (init_data_start - init_text_start) >> PAGE_SHIFT);
	set_memory_nx(init_data_start, (rodata_start - init_data_start) >> PAGE_SHIFT);
	/* rodata section is marked readonly in mark_rodata_ro */
	set_memory_nx(rodata_start, (data_start - rodata_start) >> PAGE_SHIFT);
	set_memory_nx(data_start, (max_low - data_start) >> PAGE_SHIFT);
}

void mark_rodata_ro(void)
{
	unsigned long rodata_start = (unsigned long)__start_rodata;
	unsigned long data_start = (unsigned long)_data;

	set_memory_ro(rodata_start, (data_start - rodata_start) >> PAGE_SHIFT);

	debug_checkwx();
}
#endif

void __init paging_init(void)
{
	setup_vm_final();
	setup_zero_page();
}

void __init misc_mem_init(void)
{
	arch_numa_init();
	sparse_init();
	zone_sizes_init();
	memblock_dump_all();
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node,
			       struct vmem_altmap *altmap)
{
	return vmemmap_populate_basepages(start, end, node, NULL);
}
#endif

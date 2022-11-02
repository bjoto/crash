/* riscv64.c - core analysis suite
 *
 * Copyright (C) 2022 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "defs.h"
#ifdef RISCV64

#include <elf.h>
#include <math.h>

static ulong riscv64_get_page_size(void);
static int riscv64_vtop_3level_4k(ulong *pgd, ulong vaddr,
				   physaddr_t *paddr, int verbose);
static int riscv64_vtop_4level_4k(ulong *pgd, ulong vaddr,
				   physaddr_t *paddr, int verbose);
static int riscv64_vtop_5level_4k(ulong *pgd, ulong vaddr,
				   physaddr_t *paddr, int verbose);
static void riscv64_page_type_init(void);
static int riscv64_is_kvaddr(ulong vaddr);
static int riscv64_is_uvaddr(ulong vaddr, struct task_context *tc);
static int riscv64_uvtop(struct task_context *tc, ulong vaddr,
			  physaddr_t *paddr, int verbose);
static int riscv64_kvtop(struct task_context *tc, ulong kvaddr,
			  physaddr_t *paddr, int verbose);
static void riscv64_cmd_mach(void);
static int riscv64_translate_pte(ulong, void *, ulonglong);
static int riscv64_init_active_task_regs(void);
static int riscv64_get_crash_notes(void);
static int riscv64_get_elf_notes(void);
static void riscv64_get_va_range(struct machine_specific *ms);
static void riscv64_get_va_bits(struct machine_specific *ms);
static void riscv64_get_struct_page_size(struct machine_specific *ms);

#define REG_FMT 	"%016lx"
#define SZ_2G		0x80000000

/*
 * Holds registers during the crash.
 */
static struct riscv64_register *panic_task_regs;

/* from arch/riscv/include/asm/stacktrace.h */
struct stackframe {
	ulong fp;
	ulong ra;
};

static struct machine_specific riscv64_machine_specific = {
	._page_present = (1 << 0),
	._page_read = (1 << 1),
	._page_write = (1 << 2),
	._page_exec = (1 << 3),
	._page_user = (1 << 4),
	._page_global = (1 << 5),
	._page_accessed = (1 << 6),
	._page_dirty = (1 << 7),
	._page_soft = (1 << 8),

	.va_bits = 0,
	.struct_page_size = 0,
};

static void
pt_level_alloc(char **lvl, char *name)
{
	size_t sz = PAGESIZE();
	void *pointer = malloc(sz);

	if (!pointer)
		error(FATAL, name);
	*lvl = pointer;
}

static ulong
riscv64_get_page_size(void)
{
	return memory_page_size();
}

static ulong
riscv64_vmalloc_start(void)
{
	return ((ulong)VMALLOC_START);
}

/* Get the size of struct page {} */
static void riscv64_get_struct_page_size(struct machine_specific *ms)
{
	char *string;

	string = pc->read_vmcoreinfo("SIZE(page)");
	if (string) {
		ms->struct_page_size = atol(string);
		free(string);
	}
}

static void
riscv64_cmd_mach(void)
{
	/* TODO: */
}

static int
riscv64_verify_symbol(const char *name, ulong value, char type)
{
	/* TODO: */
	return TRUE;
}

void
riscv64_dump_machdep_table(ulong arg)
{
	/* TODO: */
}

static ulong
riscv64_processor_speed(void)
{
	/* TODO: */
	return 0;
}

static unsigned long riscv64_get_kernel_version(void)
{
	char *string;
	char buf[BUFSIZE];
	char *p1, *p2;

	if (THIS_KERNEL_VERSION)
		return THIS_KERNEL_VERSION;

	string = pc->read_vmcoreinfo("OSRELEASE");
	if (string) {
		strcpy(buf, string);

		p1 = p2 = buf;
		while (*p2 != '.')
			p2++;
		*p2 = NULLCHAR;
		kt->kernel_version[0] = atoi(p1);

		p1 = ++p2;
		while (*p2 != '.')
			p2++;
		*p2 = NULLCHAR;
		kt->kernel_version[1] = atoi(p1);

		p1 = ++p2;
		while ((*p2 >= '0') && (*p2 <= '9'))
			p2++;
		*p2 = NULLCHAR;
		kt->kernel_version[2] = atoi(p1);
		free(string);
	}
	return THIS_KERNEL_VERSION;
}

static void
riscv64_get_phys_ram_base(struct machine_specific *ms)
{
	unsigned long kernel_version = riscv64_get_kernel_version();

	/*
	 * phys_ram_base is defined in Linux kernel since 5.14.
	 */
	if (kernel_version >= LINUX(5,14,0)) {
		char *string;
		if ((string = pc->read_vmcoreinfo("NUMBER(phys_ram_base)"))) {
			ms->phys_base = atol(string);
			free(string);
		} else
			error(FATAL, "cannot read phys_ram_base\n");
	} else
		/*
		 * For qemu rv64 env and hardware platform, default phys base
		 * may different, eg,
		 *	hardware platform: 0x200000
		 *	qemu   rv64   env: 0x80200000
		 *
		 * But we only can set one default value, in this case, qemu
		 * rv64 env may can't work.
		 */
		ms->phys_base = 0x200000;
}

static void riscv64_get_va_bits(struct machine_specific *ms)
{
	unsigned long kernel_version = riscv64_get_kernel_version();

	/*
	 * VA_BITS is defined in Linux kernel since 5.17. So we use the
	 * default va bits 39 when Linux version < 5.17.
	 */
	if (kernel_version >= LINUX(5,17,0)) {
		char *string;
		if ((string = pc->read_vmcoreinfo("NUMBER(VA_BITS)"))) {
			ms->va_bits = atol(string);
			free(string);
		}
	} else
		ms->va_bits = 39;
}

static void riscv64_get_va_range(struct machine_specific *ms)
{
	unsigned long kernel_version = riscv64_get_kernel_version();
	char *string;

	if ((string = pc->read_vmcoreinfo("NUMBER(PAGE_OFFSET)"))) {
		ms->page_offset = htol(string, QUIET, NULL);
		free(string);
	} else
		goto error;

	if ((string = pc->read_vmcoreinfo("NUMBER(VMALLOC_START)"))) {
		ms->vmalloc_start_addr = htol(string, QUIET, NULL);
		free(string);
	} else
		goto error;

	if ((string = pc->read_vmcoreinfo("NUMBER(VMALLOC_END)"))) {
		ms->vmalloc_end = htol(string, QUIET, NULL);
                free(string);
	} else
		goto error;

	if ((string = pc->read_vmcoreinfo("NUMBER(VMEMMAP_START)"))) {
		ms->vmemmap_vaddr = htol(string, QUIET, NULL);
		free(string);
	} else
		goto error;

	if ((string = pc->read_vmcoreinfo("NUMBER(VMEMMAP_END)"))) {
		ms->vmemmap_end = htol(string, QUIET, NULL);
		free(string);
	} else
		goto error;

	if ((string = pc->read_vmcoreinfo("NUMBER(KERNEL_LINK_ADDR)"))) {
		ms->kernel_link_addr = htol(string, QUIET, NULL);
		free(string);
	} else
		goto error;

	/*
	 * From Linux 5.13, the kernel mapping is moved to the last 2GB
	 * of the address space, modules use the 2GB memory range right
	 * before the kernel. Before Linux 5.13, modules area is embedded
	 * in vmalloc area.
	 *
	 */
	if (kernel_version >= LINUX(5,13,0)) {
		if ((string = pc->read_vmcoreinfo("NUMBER(MODULES_VADDR)"))) {
			ms->modules_vaddr = htol(string, QUIET, NULL);
			free(string);
		} else
			goto error;

		if ((string = pc->read_vmcoreinfo("NUMBER(MODULES_END)"))) {
			ms->modules_end = htol(string, QUIET, NULL);
			free(string);
		} else
			goto error;
	} else {
		ms->modules_vaddr = ms->vmalloc_start_addr;
		ms->modules_end = ms->vmalloc_end;
	}

	if (CRASHDEBUG(1)) {
		fprintf(fp, "vmemmap	: 0x%lx - 0x%lx\n",
			ms->vmemmap_vaddr, ms->vmemmap_end);
		fprintf(fp, "vmalloc	: 0x%lx - 0x%lx\n",
			ms->vmalloc_start_addr, ms->vmalloc_end);
		fprintf(fp, "mudules	: 0x%lx - 0x%lx\n",
			ms->modules_vaddr, ms->modules_end);
		fprintf(fp, "lowmem	: 0x%lx -\n", ms->page_offset);
		fprintf(fp, "kernel link addr	: 0x%lx\n",
			ms->kernel_link_addr);
	}
	return;
error:
	error(FATAL, "cannot get vm layout\n");
}

static int
riscv64_is_kvaddr(ulong vaddr)
{
	if (IS_VMALLOC_ADDR(vaddr))
		return TRUE;

	return (vaddr >= machdep->kvbase);
}

static int
riscv64_is_uvaddr(ulong vaddr, struct task_context *unused)
{
	if (IS_VMALLOC_ADDR(vaddr))
		return FALSE;

	return (vaddr < machdep->kvbase);
}

static int
riscv64_is_task_addr(ulong task)
{
	if (tt->flags & THREAD_INFO)
		return IS_KVADDR(task);

	return (IS_KVADDR(task) && ALIGNED_STACK_OFFSET(task) == 0);
}

static int
riscv64_get_smp_cpus(void)
{
	return (get_cpus_present() > 0) ? get_cpus_present() : kt->cpus;
}

/*
 *  Include both vmalloc'd and module address space as VMALLOC space.
 */
int
riscv64_IS_VMALLOC_ADDR(ulong vaddr)
{
	return ((vaddr >= VMALLOC_START && vaddr <= VMALLOC_END) ||
		(vaddr >= VMEMMAP_VADDR && vaddr <= VMEMMAP_END) ||
		(vaddr >= MODULES_VADDR && vaddr <= MODULES_END));
}

/*
 * Translate a PTE, returning TRUE if the page is present.
 * If a physaddr pointer is passed in, don't print anything.
 */
static int
riscv64_translate_pte(ulong pte, void *physaddr, ulonglong unused)
{
	char ptebuf[BUFSIZE];
	char physbuf[BUFSIZE];
	char buf[BUFSIZE];
	int page_present;
	int len1, len2, others;
	ulong paddr;

	paddr = PTOB(pte >> _PAGE_PFN_SHIFT);
	page_present = !!(pte & _PAGE_PRESENT);

	if (physaddr) {
		*(ulong *)physaddr = paddr;
		return page_present;
	}

	sprintf(ptebuf, "%lx", pte);
	len1 = MAX(strlen(ptebuf), strlen("PTE"));
	fprintf(fp, "%s  ", mkstring(buf, len1, CENTER | LJUST, "PTE"));

	if (!page_present)
		return page_present;

	sprintf(physbuf, "%lx", paddr);
	len2 = MAX(strlen(physbuf), strlen("PHYSICAL"));
	fprintf(fp, "%s  ", mkstring(buf, len2, CENTER | LJUST, "PHYSICAL"));

	fprintf(fp, "FLAGS\n");
	fprintf(fp, "%s  %s  ",
		mkstring(ptebuf, len1, CENTER | RJUST, NULL),
		mkstring(physbuf, len2, CENTER | RJUST, NULL));

	fprintf(fp, "(");
	others = 0;

#define CHECK_PAGE_FLAG(flag)				\
	if ((_PAGE_##flag) && (pte & _PAGE_##flag))	\
		fprintf(fp, "%s" #flag, others++ ? "|" : "")
	if (pte) {
		CHECK_PAGE_FLAG(PRESENT);
		CHECK_PAGE_FLAG(READ);
		CHECK_PAGE_FLAG(WRITE);
		CHECK_PAGE_FLAG(EXEC);
		CHECK_PAGE_FLAG(USER);
		CHECK_PAGE_FLAG(GLOBAL);
		CHECK_PAGE_FLAG(ACCESSED);
		CHECK_PAGE_FLAG(DIRTY);
		CHECK_PAGE_FLAG(SOFT);
	} else {
		fprintf(fp, "no mapping");
	}

	fprintf(fp, ")\n");

	return page_present;
}

static void
riscv64_page_type_init(void)
{
	ulong va_bits = machdep->machspec->va_bits;

	/*
	 * For RISCV64 arch, any level of PTE may be a leaf PTE,
	 * so in addition to 4KiB pages,
	 * Sv39 supports 2 MiB megapages, 1 GiB gigapages;
	 * Sv48 supports 2 MiB megapages, 1 GiB gigapages, 512 GiB terapages;
	 * Sv57 supports 2 MiB megapages, 1 GiB gigapages, 512 GiB terapages, and 256 TiB petapages.
	 *
	 * refs to riscv-privileged spec.
	 *
	 * We just support 4KiB, 2MiB, 1GiB now.
	 */
	switch (machdep->pagesize)
	{
	case 0x1000:		// 4 KiB
		machdep->flags |= (va_bits == 57 ? VM_L5_4K :
				  (va_bits == 48 ? VM_L4_4K : VM_L3_4K));
		break;
	case 0x200000:		// 2 MiB
		/* TODO: */
	case 0x40000000: 	// 1 GiB
		/* TODO: */
	default:
		if (machdep->pagesize)
			error(FATAL, "invalid/unsupported page size: %d\n",
			      machdep->pagesize);
		else
			error(FATAL, "cannot determine page size\n");
	}
}

static int
riscv64_vtop_3level_4k(ulong *pgd, ulong vaddr, physaddr_t *paddr, int verbose)
{
	ulong *pgd_ptr, pgd_val;
	ulong pmd_val;
	ulong pte_val, pte_pfn;
	ulong pt_phys;

	/* PGD */
	pgd_ptr = pgd + pgd_index_l3_4k(vaddr);
	FILL_PGD(pgd, KVADDR, PAGESIZE());
	pgd_val = ULONG(machdep->pgd + PAGEOFFSET(pgd_ptr));
	if (verbose)
		fprintf(fp, "   PGD: %lx => %lx\n", (ulong)pgd_ptr, pgd_val);
	if (!pgd_val)
		goto no_page;
	pgd_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pgd_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PMD */
	FILL_PMD(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pmd_val = ULONG(machdep->pmd + PAGEOFFSET(sizeof(pmd_t) *
			pmd_index_l3_4k(vaddr)));
	if (verbose)
		fprintf(fp, "  PMD: %016lx => %016lx\n", pt_phys, pmd_val);
	if (!pmd_val)
		goto no_page;
	pmd_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pmd_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PTE */
	FILL_PTBL(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pte_val = ULONG(machdep->ptbl + PAGEOFFSET(sizeof(pte_t) *
			pte_index_l3_4k(vaddr)));
	if (verbose)
		fprintf(fp, "   PTE: %lx => %lx\n", pt_phys, pte_val);
	if (!pte_val)
		goto no_page;
	pte_val &= PTE_PFN_PROT_MASK;
	pte_pfn = pte_val >> _PAGE_PFN_SHIFT;

	if (!(pte_val & _PAGE_PRESENT)) {
		if (verbose) {
			fprintf(fp, "\n");
			riscv64_translate_pte((ulong)pte_val, 0, 0);
		}
		fprintf(fp, " PAGE: %016lx not present\n\n", PAGEBASE(*paddr));
		return FALSE;
	}

	*paddr = PTOB(pte_pfn) + PAGEOFFSET(vaddr);

	if (verbose) {
		fprintf(fp, " PAGE: %016lx\n\n", PAGEBASE(*paddr));
		riscv64_translate_pte(pte_val, 0, 0);
	}

	return TRUE;
no_page:
	fprintf(fp, "invalid\n");
	return FALSE;
}

static int
riscv64_vtop_4level_4k(ulong *pgd, ulong vaddr, physaddr_t *paddr, int verbose)
{
	ulong *pgd_ptr, pgd_val;
	ulong pud_val;
	ulong pmd_val;
	ulong pte_val, pte_pfn;
	ulong pt_phys;

	/* PGD */
	pgd_ptr = pgd + pgd_index_l4_4k(vaddr);
	FILL_PGD(pgd, KVADDR, PAGESIZE());
	pgd_val = ULONG(machdep->pgd + PAGEOFFSET(pgd_ptr));
	if (verbose)
		fprintf(fp, "   PGD: %lx => %lx\n", (ulong)pgd_ptr, pgd_val);
	if (!pgd_val)
		goto no_page;
	pgd_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pgd_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PUD */
	FILL_PUD(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pud_val = ULONG(machdep->pud + PAGEOFFSET(sizeof(pud_t) *
			pud_index_l4_4k(vaddr)));
	if (verbose)
		fprintf(fp, "  PUD: %016lx => %016lx\n", pt_phys, pud_val);
	if (!pud_val)
		goto no_page;
	pud_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pud_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PMD */
	FILL_PMD(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pmd_val = ULONG(machdep->pmd + PAGEOFFSET(sizeof(pmd_t) *
			pmd_index_l4_4k(vaddr)));
	if (verbose)
		fprintf(fp, "  PMD: %016lx => %016lx\n", pt_phys, pmd_val);
	if (!pmd_val)
		goto no_page;
	pmd_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pmd_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PTE */
	FILL_PTBL(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pte_val = ULONG(machdep->ptbl + PAGEOFFSET(sizeof(pte_t) *
			pte_index_l4_4k(vaddr)));
	if (verbose)
		fprintf(fp, "   PTE: %lx => %lx\n", pt_phys, pte_val);
	if (!pte_val)
		goto no_page;
	pte_val &= PTE_PFN_PROT_MASK;
	pte_pfn = pte_val >> _PAGE_PFN_SHIFT;

	if (!(pte_val & _PAGE_PRESENT)) {
		if (verbose) {
			fprintf(fp, "\n");
			riscv64_translate_pte((ulong)pte_val, 0, 0);
		}
		fprintf(fp, " PAGE: %016lx not present\n\n", PAGEBASE(*paddr));
		return FALSE;
	}

	*paddr = PTOB(pte_pfn) + PAGEOFFSET(vaddr);

	if (verbose) {
		fprintf(fp, " PAGE: %016lx\n\n", PAGEBASE(*paddr));
		riscv64_translate_pte(pte_val, 0, 0);
	}

	return TRUE;
no_page:
	fprintf(fp, "invalid\n");
	return FALSE;
}

static int
riscv64_vtop_5level_4k(ulong *pgd, ulong vaddr, physaddr_t *paddr, int verbose)
{
	ulong *pgd_ptr, pgd_val;
	ulong p4d_val;
	ulong pud_val;
	ulong pmd_val;
	ulong pte_val, pte_pfn;
	ulong pt_phys;

	/* PGD */
	pgd_ptr = pgd + pgd_index_l5_4k(vaddr);
	FILL_PGD(pgd, KVADDR, PAGESIZE());
	pgd_val = ULONG(machdep->pgd + PAGEOFFSET(pgd_ptr));
	if (verbose)
		fprintf(fp, "   PGD: %lx => %lx\n", (ulong)pgd_ptr, pgd_val);
	if (!pgd_val)
		goto no_page;
	pgd_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pgd_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* P4D */
	FILL_P4D(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	p4d_val = ULONG(machdep->machspec->p4d + PAGEOFFSET(sizeof(p4d_t) *
			p4d_index_l5_4k(vaddr)));
	if (verbose)
		fprintf(fp, "  P4D: %016lx => %016lx\n", pt_phys, p4d_val);
	if (!p4d_val)
		goto no_page;
	p4d_val &= PTE_PFN_PROT_MASK;
	pt_phys = (p4d_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PUD */
	FILL_PUD(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pud_val = ULONG(machdep->pud + PAGEOFFSET(sizeof(pud_t) *
			pud_index_l5_4k(vaddr)));
	if (verbose)
		fprintf(fp, "  PUD: %016lx => %016lx\n", pt_phys, pud_val);
	if (!pud_val)
		goto no_page;
	pud_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pud_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PMD */
	FILL_PMD(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pmd_val = ULONG(machdep->pmd + PAGEOFFSET(sizeof(pmd_t) *
			pmd_index_l4_4k(vaddr)));
	if (verbose)
		fprintf(fp, "  PMD: %016lx => %016lx\n", pt_phys, pmd_val);
	if (!pmd_val)
		goto no_page;
	pmd_val &= PTE_PFN_PROT_MASK;
	pt_phys = (pmd_val >> _PAGE_PFN_SHIFT) << PAGESHIFT();

	/* PTE */
	FILL_PTBL(PAGEBASE(pt_phys), PHYSADDR, PAGESIZE());
	pte_val = ULONG(machdep->ptbl + PAGEOFFSET(sizeof(pte_t) *
			pte_index_l4_4k(vaddr)));
	if (verbose)
		fprintf(fp, "   PTE: %lx => %lx\n", pt_phys, pte_val);
	if (!pte_val)
		goto no_page;
	pte_val &= PTE_PFN_PROT_MASK;
	pte_pfn = pte_val >> _PAGE_PFN_SHIFT;

	if (!(pte_val & _PAGE_PRESENT)) {
		if (verbose) {
			fprintf(fp, "\n");
			riscv64_translate_pte((ulong)pte_val, 0, 0);
		}
		printf("!_PAGE_PRESENT\n");
		return FALSE;
	}

	*paddr = PTOB(pte_pfn) + PAGEOFFSET(vaddr);

	if (verbose) {
		fprintf(fp, " PAGE: %016lx\n\n", PAGEBASE(*paddr));
		riscv64_translate_pte(pte_val, 0, 0);
	}

	return TRUE;
no_page:
	fprintf(fp, "invalid\n");
	return FALSE;
}

static int
riscv64_init_active_task_regs(void)
{
	int retval;

	retval = riscv64_get_crash_notes();
	if (retval == TRUE)
		return retval;

	return riscv64_get_elf_notes();
}

/*
 * Retrieve task registers for the time of the crash.
 */
static int
riscv64_get_crash_notes(void)
{
	struct machine_specific *ms = machdep->machspec;
	ulong crash_notes;
	Elf64_Nhdr *note;
	ulong offset;
	char *buf, *p;
	ulong *notes_ptrs;
	ulong i;

	/*
	 * crash_notes contains per cpu memory for storing cpu states
	 * in case of system crash.
	 */
	if (!symbol_exists("crash_notes"))
		return FALSE;

	crash_notes = symbol_value("crash_notes");

	notes_ptrs = (ulong *)GETBUF(kt->cpus*sizeof(notes_ptrs[0]));

	/*
	 * Read crash_notes for the first CPU. crash_notes are in standard ELF
	 * note format.
	 */
	if (!readmem(crash_notes, KVADDR, &notes_ptrs[kt->cpus-1],
	    sizeof(notes_ptrs[kt->cpus-1]), "crash_notes",
		     RETURN_ON_ERROR)) {
		error(WARNING, "cannot read crash_notes\n");
		FREEBUF(notes_ptrs);
		return FALSE;
	}

	if (symbol_exists("__per_cpu_offset")) {

		/*
		 * Add __per_cpu_offset for each cpu to form the pointer to the notes
		 */
		for (i = 0; i < kt->cpus; i++)
			notes_ptrs[i] = notes_ptrs[kt->cpus-1] + kt->__per_cpu_offset[i];
	}

	buf = GETBUF(SIZE(note_buf));

	if (!(panic_task_regs = calloc((size_t)kt->cpus, sizeof(*panic_task_regs))))
		error(FATAL, "cannot calloc panic_task_regs space\n");

	for (i = 0; i < kt->cpus; i++) {

		if (!readmem(notes_ptrs[i], KVADDR, buf, SIZE(note_buf), "note_buf_t",
			     RETURN_ON_ERROR)) {
			error(WARNING,
				"cannot find NT_PRSTATUS note for cpu: %d\n", i);
			goto fail;
		}

		/*
		 * Do some sanity checks for this note before reading registers from it.
		 */
		note = (Elf64_Nhdr *)buf;
		p = buf + sizeof(Elf64_Nhdr);

		/*
		 * dumpfiles created with qemu won't have crash_notes, but there will
		 * be elf notes; dumpfiles created by kdump do not create notes for
		 * offline cpus.
		 */
		if (note->n_namesz == 0 && (DISKDUMP_DUMPFILE() || KDUMP_DUMPFILE())) {
			if (DISKDUMP_DUMPFILE())
				note = diskdump_get_prstatus_percpu(i);
			else if (KDUMP_DUMPFILE())
				note = netdump_get_prstatus_percpu(i);
			if (note) {
				/*
				 * SIZE(note_buf) accounts for a "final note", which is a
				 * trailing empty elf note header.
				 */
				long notesz = SIZE(note_buf) - sizeof(Elf64_Nhdr);

				if (sizeof(Elf64_Nhdr) + roundup(note->n_namesz, 4) +
				    note->n_descsz == notesz)
					BCOPY((char *)note, buf, notesz);
			} else {
				error(WARNING,
					"cannot find NT_PRSTATUS note for cpu: %d\n", i);
				continue;
			}
		}

		/*
		 * Check the sanity of NT_PRSTATUS note only for each online cpu.
		 */
		if (note->n_type != NT_PRSTATUS) {
			error(WARNING, "invalid NT_PRSTATUS note (n_type != NT_PRSTATUS)\n");
			goto fail;
		}
		if (!STRNEQ(p, "CORE")) {
			error(WARNING, "invalid NT_PRSTATUS note (name != \"CORE\"\n");
			goto fail;
		}

		/*
		 * Find correct location of note data. This contains elf_prstatus
		 * structure which has registers etc. for the crashed task.
		 */
		offset = sizeof(Elf64_Nhdr);
		offset = roundup(offset + note->n_namesz, 4);
		p = buf + offset; /* start of elf_prstatus */

		BCOPY(p + OFFSET(elf_prstatus_pr_reg), &panic_task_regs[i],
		      sizeof(panic_task_regs[i]));
	}

	/*
	 * And finally we have the registers for the crashed task. This is
	 * used later on when dumping backtrace.
	 */
	ms->crash_task_regs = panic_task_regs;

	FREEBUF(buf);
	FREEBUF(notes_ptrs);
	return TRUE;

fail:
	FREEBUF(buf);
	FREEBUF(notes_ptrs);
	free(panic_task_regs);
	return FALSE;
}

static int
riscv64_get_elf_notes(void)
{
	struct machine_specific *ms = machdep->machspec;
	int i;

	if (!DISKDUMP_DUMPFILE() && !KDUMP_DUMPFILE())
		return FALSE;

	panic_task_regs = calloc(kt->cpus, sizeof(*panic_task_regs));
	if (!panic_task_regs)
		error(FATAL, "cannot calloc panic_task_regs space\n");

	for (i = 0; i < kt->cpus; i++) {
		Elf64_Nhdr *note = NULL;
		size_t len;

		if (DISKDUMP_DUMPFILE())
			note = diskdump_get_prstatus_percpu(i);
		else if (KDUMP_DUMPFILE())
			note = netdump_get_prstatus_percpu(i);

		if (!note) {
			error(WARNING,
				"cannot find NT_PRSTATUS note for cpu: %d\n", i);
			continue;
		}

		len = sizeof(Elf64_Nhdr);
		len = roundup(len + note->n_namesz, 4);

		BCOPY((char *)note + len + OFFSET(elf_prstatus_pr_reg),
		      &panic_task_regs[i], sizeof(panic_task_regs[i]));
	}

	ms->crash_task_regs = panic_task_regs;

	return TRUE;
}

/*
 * Translates a user virtual address to its physical address.
 */
static int
riscv64_uvtop(struct task_context *tc, ulong uvaddr, physaddr_t *paddr, int verbose)
{
	ulong mm, active_mm;
	ulong *pgd;

	if (!tc)
		error(FATAL, "current context invalid\n");

	*paddr = 0;

	if (is_kernel_thread(tc->task) && IS_KVADDR(uvaddr)) {
		readmem(tc->task + OFFSET(task_struct_active_mm),
			KVADDR, &active_mm, sizeof(void *),
			"task active_mm contents", FAULT_ON_ERROR);

		if (!active_mm)
			error(FATAL,
			      "no active_mm for this kernel thread\n");

		readmem(active_mm + OFFSET(mm_struct_pgd),
			KVADDR, &pgd, sizeof(long),
			"mm_struct pgd", FAULT_ON_ERROR);
	} else {
		if ((mm = task_mm(tc->task, TRUE)))
			pgd = ULONG_PTR(tt->mm_struct + OFFSET(mm_struct_pgd));
		else
			readmem(tc->mm_struct + OFFSET(mm_struct_pgd),
				KVADDR, &pgd, sizeof(long), "mm_struct pgd",
				FAULT_ON_ERROR);
	}

	switch (machdep->flags & VM_FLAGS)
	{
	case VM_L3_4K:
		return riscv64_vtop_3level_4k(pgd, uvaddr, paddr, verbose);
	case VM_L4_4K:
		return riscv64_vtop_4level_4k(pgd, uvaddr, paddr, verbose);
	case VM_L5_4K:
		return riscv64_vtop_5level_4k(pgd, uvaddr, paddr, verbose);
	default:
		return FALSE;
	}
}

static int
riscv64_kvtop(struct task_context *tc, ulong kvaddr, physaddr_t *paddr, int verbose)
{
	ulong kernel_pgd;

	if (!IS_KVADDR(kvaddr))
		return FALSE;

	if (!vt->vmalloc_start) {
		*paddr = VTOP(kvaddr);
		return TRUE;
	}

	if (!IS_VMALLOC_ADDR(kvaddr)) {
		*paddr = VTOP(kvaddr);
		if (!verbose)
			return TRUE;
	}

	kernel_pgd = vt->kernel_pgd[0];
	*paddr = 0;

	switch (machdep->flags & VM_FLAGS)
	{
	case VM_L3_4K:
		return riscv64_vtop_3level_4k((ulong *)kernel_pgd, kvaddr, paddr, verbose);
	case VM_L4_4K:
		return riscv64_vtop_4level_4k((ulong *)kernel_pgd, kvaddr, paddr, verbose);
	case VM_L5_4K:
		return riscv64_vtop_5level_4k((ulong *)kernel_pgd, kvaddr, paddr, verbose);
	default:
		return FALSE;
	}
}

void
riscv64_init(int when)
{
	switch (when) {
	case SETUP_ENV:
		machdep->process_elf_notes = process_elf64_notes;
		break;

	case PRE_SYMTAB:
		machdep->verify_symbol = riscv64_verify_symbol;
		machdep->machspec = &riscv64_machine_specific;
		if (pc->flags & KERNEL_DEBUG_QUERY)
			return;

		machdep->verify_paddr = generic_verify_paddr;
		machdep->ptrs_per_pgd = PTRS_PER_PGD;
		break;

	case PRE_GDB:
		machdep->pagesize = riscv64_get_page_size();
		machdep->pageshift = ffs(machdep->pagesize) - 1;
		machdep->pageoffset = machdep->pagesize - 1;
		machdep->pagemask = ~((ulonglong)machdep->pageoffset);
		machdep->stacksize = machdep->pagesize << THREAD_SIZE_ORDER;

		riscv64_get_phys_ram_base(machdep->machspec);
		riscv64_get_struct_page_size(machdep->machspec);
		riscv64_get_va_bits(machdep->machspec);
		riscv64_get_va_range(machdep->machspec);

		pt_level_alloc(&machdep->pgd, "cannot malloc pgd space.");
		pt_level_alloc(&machdep->machspec->p4d, "cannot malloc p4d space.");
		pt_level_alloc(&machdep->pud, "cannot malloc pud space.");
		pt_level_alloc(&machdep->pmd, "cannot malloc pmd space.");
		pt_level_alloc(&machdep->ptbl, "cannot malloc ptbl space.");

		machdep->last_pgd_read = 0;
		machdep->machspec->last_p4d_read = 0;
		machdep->last_pud_read = 0;
		machdep->last_pmd_read = 0;
		machdep->last_ptbl_read = 0;

		machdep->kvbase = machdep->machspec->page_offset;
		machdep->identity_map_base = machdep->kvbase;
		machdep->is_kvaddr = riscv64_is_kvaddr;
		machdep->is_uvaddr = riscv64_is_uvaddr;
		machdep->uvtop = riscv64_uvtop;
		machdep->kvtop = riscv64_kvtop;
		machdep->cmd_mach = riscv64_cmd_mach;

		machdep->vmalloc_start = riscv64_vmalloc_start;
		machdep->processor_speed = riscv64_processor_speed;
		machdep->get_stackbase = generic_get_stackbase;
		machdep->get_stacktop = generic_get_stacktop;
		machdep->translate_pte = riscv64_translate_pte;
		machdep->memory_size = generic_memory_size;
		machdep->is_task_addr = riscv64_is_task_addr;
		machdep->get_smp_cpus = riscv64_get_smp_cpus;
		machdep->value_to_symbol = generic_machdep_value_to_symbol;
		machdep->dis_filter = generic_dis_filter;
		machdep->show_interrupts = generic_show_interrupts;
		machdep->get_irq_affinity = generic_get_irq_affinity;
		machdep->init_kernel_pgd = NULL; /* pgd set by symbol_value("swapper_pg_dir") */
		break;

	case POST_GDB:
		machdep->section_size_bits = _SECTION_SIZE_BITS;
		machdep->max_physmem_bits = _MAX_PHYSMEM_BITS;
		riscv64_page_type_init();

		if (!machdep->hz)
			machdep->hz = 250;

		if (symbol_exists("irq_desc"))
			ARRAY_LENGTH_INIT(machdep->nr_irqs, irq_desc,
					  "irq_desc", NULL, 0);
		else if (kernel_symbol_exists("nr_irqs"))
			get_symbol_data("nr_irqs", sizeof(unsigned int),
					&machdep->nr_irqs);

		MEMBER_OFFSET_INIT(elf_prstatus_pr_reg, "elf_prstatus",
				   "pr_reg");

		STRUCT_SIZE_INIT(note_buf, "note_buf_t");
		break;

	case POST_VM:
		/*
		 * crash_notes contains machine specific information about the
		 * crash. In particular, it contains CPU registers at the time
		 * of the crash. We need this information to extract correct
		 * backtraces from the panic task.
		 */
		if (!ACTIVE() && !riscv64_init_active_task_regs())
			error(WARNING,
				"cannot retrieve registers for active task%s\n\n",
				kt->cpus > 1 ? "s" : "");
		break;
	}
}

/*
 * 'help -r' command output
 */
void
riscv64_display_regs_from_elf_notes(int cpu, FILE *ofp)
{
}

#else /* !RISCV64 */

void
riscv64_display_regs_from_elf_notes(int cpu, FILE *ofp)
{
	return;
}

#endif /* !RISCV64 */

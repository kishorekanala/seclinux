/*
 * recordmcount.c: construct a table of the locations of calls to 'mcount'
 * so that ftrace can find them quickly.
 * Copyright 2009 John F. Reiser <jreiser@BitWagon.com>.  All rights reserved.
 * Licensed under the GNU General Public License, version 2 (GPLv2).
 *
 * Restructured to fit Linux format, as well as other updates:
 *  Copyright 2010 Steven Rostedt <srostedt@redhat.com>, Red Hat Inc.
 */

/*
 * Strategy: alter the .o file in-place.
 *
 * Append a new STRTAB that has the new section names, followed by a new array
 * ElfXX_Shdr[] that has the new section headers, followed by the section
 * contents for __mcount_loc and its relocations.  The old shstrtab strings,
 * and the old ElfXX_Shdr[] array, remain as "garbage" (commonly, a couple
 * kilobytes.)  Subsequent processing by /bin/ld (or the kernel module loader)
 * will ignore the garbage regions, because they are not designated by the
 * new .e_shoff nor the new ElfXX_Shdr[].  [In order to remove the garbage,
 * then use "ld -r" to create a new file that omits the garbage.]
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd_map;	/* File descriptor for file being modified. */
static int mmap_failed; /* Boolean flag. */
static void *ehdr_curr; /* current ElfXX_Ehdr *  for resource cleanup */
static char gpfx;	/* prefix for global symbol name (sometimes '_') */
static struct stat sb;	/* Remember .st_size, etc. */
static jmp_buf jmpenv;	/* setjmp/longjmp per-file error escape */

/* setjmp() return values */
enum {
	SJ_SETJMP = 0,  /* hardwired first return */
	SJ_FAIL,
	SJ_SUCCEED
};

/* Per-file resource cleanup when multiple files. */
static void
cleanup(void)
{
	if (!mmap_failed)
		munmap(ehdr_curr, sb.st_size);
	else
		free(ehdr_curr);
	close(fd_map);
}

static void __attribute__((noreturn))
fail_file(void)
{
	cleanup();
	longjmp(jmpenv, SJ_FAIL);
}

static void __attribute__((noreturn))
succeed_file(void)
{
	cleanup();
	longjmp(jmpenv, SJ_SUCCEED);
}

/* ulseek, uread, ...:  Check return value for errors. */

static off_t
ulseek(int const fd, off_t const offset, int const whence)
{
	off_t const w = lseek(fd, offset, whence);
	if ((off_t)-1 == w) {
		perror("lseek");
		fail_file();
	}
	return w;
}

static size_t
uread(int const fd, void *const buf, size_t const count)
{
	size_t const n = read(fd, buf, count);
	if (n != count) {
		perror("read");
		fail_file();
	}
	return n;
}

static size_t
uwrite(int const fd, void const *const buf, size_t const count)
{
	size_t const n = write(fd, buf, count);
	if (n != count) {
		perror("write");
		fail_file();
	}
	return n;
}

static void *
umalloc(size_t size)
{
	void *const addr = malloc(size);
	if (0 == addr) {
		fprintf(stderr, "malloc failed: %zu bytes\n", size);
		fail_file();
	}
	return addr;
}

/*
 * Get the whole file as a programming convenience in order to avoid
 * malloc+lseek+read+free of many pieces.  If successful, then mmap
 * avoids copying unused pieces; else just read the whole file.
 * Open for both read and write; new info will be appended to the file.
 * Use MAP_PRIVATE so that a few changes to the in-memory ElfXX_Ehdr
 * do not propagate to the file until an explicit overwrite at the last.
 * This preserves most aspects of consistency (all except .st_size)
 * for simultaneous readers of the file while we are appending to it.
 * However, multiple writers still are bad.  We choose not to use
 * locking because it is expensive and the use case of kernel build
 * makes multiple writers unlikely.
 */
static void *mmap_file(char const *fname)
{
	void *addr;

	fd_map = open(fname, O_RDWR);
	if (0 > fd_map || 0 > fstat(fd_map, &sb)) {
		perror(fname);
		fail_file();
	}
	if (!S_ISREG(sb.st_mode)) {
		fprintf(stderr, "not a regular file: %s\n", fname);
		fail_file();
	}
	addr = mmap(0, sb.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE,
		    fd_map, 0);
	mmap_failed = 0;
	if (MAP_FAILED == addr) {
		mmap_failed = 1;
		addr = umalloc(sb.st_size);
		uread(fd_map, addr, sb.st_size);
	}
	return addr;
}

/* w8rev, w8nat, ...: Handle endianness. */

static uint64_t w8rev(uint64_t const x)
{
	return   ((0xff & (x >> (0 * 8))) << (7 * 8))
	       | ((0xff & (x >> (1 * 8))) << (6 * 8))
	       | ((0xff & (x >> (2 * 8))) << (5 * 8))
	       | ((0xff & (x >> (3 * 8))) << (4 * 8))
	       | ((0xff & (x >> (4 * 8))) << (3 * 8))
	       | ((0xff & (x >> (5 * 8))) << (2 * 8))
	       | ((0xff & (x >> (6 * 8))) << (1 * 8))
	       | ((0xff & (x >> (7 * 8))) << (0 * 8));
}

static uint32_t w4rev(uint32_t const x)
{
	return   ((0xff & (x >> (0 * 8))) << (3 * 8))
	       | ((0xff & (x >> (1 * 8))) << (2 * 8))
	       | ((0xff & (x >> (2 * 8))) << (1 * 8))
	       | ((0xff & (x >> (3 * 8))) << (0 * 8));
}

static uint32_t w2rev(uint16_t const x)
{
	return   ((0xff & (x >> (0 * 8))) << (1 * 8))
	       | ((0xff & (x >> (1 * 8))) << (0 * 8));
}

static uint64_t w8nat(uint64_t const x)
{
	return x;
}

static uint32_t w4nat(uint32_t const x)
{
	return x;
}

static uint32_t w2nat(uint16_t const x)
{
	return x;
}

static uint64_t (*w8)(uint64_t);
static uint32_t (*w)(uint32_t);
static uint32_t (*w2)(uint16_t);

/* Names of the sections that could contain calls to mcount. */
static int
is_mcounted_section_name(char const *const txtname)
{
	return 0 == strcmp(".text",          txtname) ||
		0 == strcmp(".sched.text",    txtname) ||
		0 == strcmp(".spinlock.text", txtname) ||
		0 == strcmp(".irqentry.text", txtname) ||
		0 == strcmp(".text.unlikely", txtname);
}

/* 32 bit and 64 bit are very similar */
#include "recordmcount.h"
#define RECORD_MCOUNT_64
#include "recordmcount.h"

/* 64-bit EM_MIPS has weird ELF64_Rela.r_info.
 * http://techpubs.sgi.com/library/manuals/4000/007-4658-001/pdf/007-4658-001.pdf
 * We interpret Table 29 Relocation Operation (Elf64_Rel, Elf64_Rela) [p.40]
 * to imply the order of the members; the spec does not say so.
 *	typedef unsigned char Elf64_Byte;
 * fails on MIPS64 because their <elf.h> already has it!
 */

typedef uint8_t myElf64_Byte;		/* Type for a 8-bit quantity.  */

union mips_r_info {
	Elf64_Xword r_info;
	struct {
		Elf64_Word r_sym;		/* Symbol index.  */
		myElf64_Byte r_ssym;		/* Special symbol.  */
		myElf64_Byte r_type3;		/* Third relocation.  */
		myElf64_Byte r_type2;		/* Second relocation.  */
		myElf64_Byte r_type;		/* First relocation.  */
	} r_mips;
};

static uint64_t MIPS64_r_sym(Elf64_Rel const *rp)
{
	return w(((union mips_r_info){ .r_info = rp->r_info }).r_mips.r_sym);
}

static void MIPS64_r_info(Elf64_Rel *const rp, unsigned sym, unsigned type)
{
	rp->r_info = ((union mips_r_info){
		.r_mips = { .r_sym = w(sym), .r_type = type }
	}).r_info;
}

static void
do_file(char const *const fname)
{
	Elf32_Ehdr *const ehdr = mmap_file(fname);
	unsigned int reltype = 0;

	ehdr_curr = ehdr;
	w = w4nat;
	w2 = w2nat;
	w8 = w8nat;
	switch (ehdr->e_ident[EI_DATA]) {
		static unsigned int const endian = 1;
	default: {
		fprintf(stderr, "unrecognized ELF data encoding %d: %s\n",
			ehdr->e_ident[EI_DATA], fname);
		fail_file();
	} break;
	case ELFDATA2LSB: {
		if (1 != *(unsigned char const *)&endian) {
			/* main() is big endian, file.o is little endian. */
			w = w4rev;
			w2 = w2rev;
			w8 = w8rev;
		}
	} break;
	case ELFDATA2MSB: {
		if (0 != *(unsigned char const *)&endian) {
			/* main() is little endian, file.o is big endian. */
			w = w4rev;
			w2 = w2rev;
			w8 = w8rev;
		}
	} break;
	}  /* end switch */
	if (0 != memcmp(ELFMAG, ehdr->e_ident, SELFMAG)
	||  ET_REL != w2(ehdr->e_type)
	||  EV_CURRENT != ehdr->e_ident[EI_VERSION]) {
		fprintf(stderr, "unrecognized ET_REL file %s\n", fname);
		fail_file();
	}

	gpfx = 0;
	switch (w2(ehdr->e_machine)) {
	default: {
		fprintf(stderr, "unrecognized e_machine %d %s\n",
			w2(ehdr->e_machine), fname);
		fail_file();
	} break;
	case EM_386:	 reltype = R_386_32;                   break;
	case EM_ARM:	 reltype = R_ARM_ABS32;                break;
	case EM_IA_64:	 reltype = R_IA64_IMM64;   gpfx = '_'; break;
	case EM_MIPS:	 /* reltype: e_class    */ gpfx = '_'; break;
	case EM_PPC:	 reltype = R_PPC_ADDR32;   gpfx = '_'; break;
	case EM_PPC64:	 reltype = R_PPC64_ADDR64; gpfx = '_'; break;
	case EM_S390:    /* reltype: e_class    */ gpfx = '_'; break;
	case EM_SH:	 reltype = R_SH_DIR32;                 break;
	case EM_SPARCV9: reltype = R_SPARC_64;     gpfx = '_'; break;
	case EM_X86_64:	 reltype = R_X86_64_64;                break;
	}  /* end switch */

	switch (ehdr->e_ident[EI_CLASS]) {
	default: {
		fprintf(stderr, "unrecognized ELF class %d %s\n",
			ehdr->e_ident[EI_CLASS], fname);
		fail_file();
	} break;
	case ELFCLASS32: {
		if (sizeof(Elf32_Ehdr) != w2(ehdr->e_ehsize)
		||  sizeof(Elf32_Shdr) != w2(ehdr->e_shentsize)) {
			fprintf(stderr,
				"unrecognized ET_REL file: %s\n", fname);
			fail_file();
		}
		if (EM_S390 == w2(ehdr->e_machine))
			reltype = R_390_32;
		if (EM_MIPS == w2(ehdr->e_machine)) {
			reltype = R_MIPS_32;
			is_fake_mcount32 = MIPS32_is_fake_mcount;
		}
		do32(ehdr, fname, reltype);
	} break;
	case ELFCLASS64: {
		Elf64_Ehdr *const ghdr = (Elf64_Ehdr *)ehdr;
		if (sizeof(Elf64_Ehdr) != w2(ghdr->e_ehsize)
		||  sizeof(Elf64_Shdr) != w2(ghdr->e_shentsize)) {
			fprintf(stderr,
				"unrecognized ET_REL file: %s\n", fname);
			fail_file();
		}
		if (EM_S390 == w2(ghdr->e_machine))
			reltype = R_390_64;
		if (EM_MIPS == w2(ghdr->e_machine)) {
			reltype = R_MIPS_64;
			Elf64_r_sym = MIPS64_r_sym;
			Elf64_r_info = MIPS64_r_info;
			is_fake_mcount64 = MIPS64_is_fake_mcount;
		}
		do64(ghdr, fname, reltype);
	} break;
	}  /* end switch */

	cleanup();
}

int
main(int argc, char const *argv[])
{
	const char ftrace[] = "kernel/trace/ftrace.o";
	int ftrace_size = sizeof(ftrace) - 1;
	int n_error = 0;  /* gcc-4.3.0 false positive complaint */

	if (argc <= 1) {
		fprintf(stderr, "usage: recordmcount file.o...\n");
		return 0;
	}

	/* Process each file in turn, allowing deep failure. */
	for (--argc, ++argv; 0 < argc; --argc, ++argv) {
		int const sjval = setjmp(jmpenv);
		int len;

		/*
		 * The file kernel/trace/ftrace.o references the mcount
		 * function but does not call it. Since ftrace.o should
		 * not be traced anyway, we just skip it.
		 */
		len = strlen(argv[0]);
		if (len >= ftrace_size &&
		    strcmp(argv[0] + (len - ftrace_size), ftrace) == 0)
			continue;

		switch (sjval) {
		default: {
			fprintf(stderr, "internal error: %s\n", argv[0]);
			exit(1);
		} break;
		case SJ_SETJMP: {  /* normal sequence */
			/* Avoid problems if early cleanup() */
			fd_map = -1;
			ehdr_curr = NULL;
			mmap_failed = 1;
			do_file(argv[0]);
		} break;
		case SJ_FAIL: {  /* error in do_file or below */
			++n_error;
		} break;
		case SJ_SUCCEED: {  /* premature success */
			/* do nothing */
		} break;
		}  /* end switch */
	}
	return !!n_error;
}


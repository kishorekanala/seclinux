/*
 * misc.c
 * 
 * This is a collection of several routines from gzip-1.0.3 
 * adapted for Linux.
 *
 * malloc by Hannu Savolainen 1993 and Matthias Urlichs 1994
 *
 * Modified for ARM Linux by Russell King
 *
 * Nicolas Pitre <nico@visuaide.com>  1999/04/14 :
 *  For this code to run directly from Flash, all constant variables must
 *  be marked with 'const' and all other variables initialized at run-time 
 *  only.  This way all non constant variables will end up in the bss segment,
 *  which should point to addresses in RAM and cleared to 0 on start.
 *  This allows for a much quicker boot time.
 */

unsigned int __machine_arch_type;

#define _LINUX_STRING_H_

#include <linux/compiler.h>	/* for inline */
#include <linux/types.h>	/* for size_t */
#include <linux/stddef.h>	/* for NULL */
#include <linux/linkage.h>
#include <asm/string.h>

#include <asm/unaligned.h>


static void putstr(const char *ptr);
extern void error(char *x);

#include <mach/uncompress.h>

#ifdef CONFIG_DEBUG_ICEDCC

#ifdef CONFIG_CPU_V6


static void icedcc_putc(int ch)
{
	int status, i = 0x4000000;

	do {
		if (--i < 0)
			return;

		asm volatile ("mrc p14, 0, %0, c0, c1, 0" : "=r" (status));
	} while (status & (1 << 29));

	asm("mcr p14, 0, %0, c0, c5, 0" : : "r" (ch));
}

#elif defined(CONFIG_CPU_V7)

static void icedcc_putc(int ch)
{
	asm(
	"wait:	mrc	p14, 0, pc, c0, c1, 0			\n\
		bcs	wait					\n\
		mcr     p14, 0, %0, c0, c5, 0			"
	: : "r" (ch));
}

#elif defined(CONFIG_CPU_XSCALE)

static void icedcc_putc(int ch)
{
	int status, i = 0x4000000;

	do {
		if (--i < 0)
			return;

		asm volatile ("mrc p14, 0, %0, c14, c0, 0" : "=r" (status));
	} while (status & (1 << 28));

	asm("mcr p14, 0, %0, c8, c0, 0" : : "r" (ch));
}

#else

static void icedcc_putc(int ch)
{
	int status, i = 0x4000000;

	do {
		if (--i < 0)
			return;

		asm volatile ("mrc p14, 0, %0, c0, c0, 0" : "=r" (status));
	} while (status & 2);

	asm("mcr p14, 0, %0, c1, c0, 0" : : "r" (ch));
}

#endif

#define putc(ch)	icedcc_putc(ch)
#endif

static void putstr(const char *ptr)
{
	char c;

	while ((c = *ptr++) != '\0') {
		if (c == '\n')
			putc('\r');
		putc(c);
	}

	flush();
}


void *memcpy(void *__dest, __const void *__src, size_t __n)
{
	int i = 0;
	unsigned char *d = (unsigned char *)__dest, *s = (unsigned char *)__src;

	for (i = __n >> 3; i > 0; i--) {
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
	}

	if (__n & 1 << 2) {
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
	}

	if (__n & 1 << 1) {
		*d++ = *s++;
		*d++ = *s++;
	}

	if (__n & 1)
		*d++ = *s++;

	return __dest;
}

/*
 * gzip delarations
 */
extern char input_data[];
extern char input_data_end[];

unsigned char *output_data;
unsigned long output_ptr;

unsigned long free_mem_ptr;
unsigned long free_mem_end_ptr;

#ifndef arch_error
#define arch_error(x)
#endif

void strreverse(char* begin, char* end) {
	
	char aux;
	
	while(end>begin)
	
		aux=*end, *end--=*begin, *begin++=aux;
	
}

char* itoa(int value, char* str, int base) 
{
	
	static char num[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	
	char* wstr=str;
	
	int sign;
	

	
	// Validate base
	
	if (base<2 || base>35){ *wstr='\0'; return NULL; }
	

	
	// Take care of sign
	
	if ((sign=value) < 0) value = -value;
	

	
	// Conversion. Number is reversed.
	
	do *wstr++ = num[value%base]; while(value/=base);
	
	if(sign<0) *wstr++='-';
	
	*wstr='\0';
	

	
	// Reverse string
	
	strreverse(str,wstr-1);
	
        return str;
}

void error(char *x)
{
	arch_error(x);

	putstr("\n\n");
	putstr(x);
	putstr("\n\n -- System halted");

	while(1);	/* Halt */
}

asmlinkage void __div0(void)
{
	error("Attempting division by 0!");
}

extern void do_decompress(u8 *input, int len, u8 *output, void (*error)(char *x));


unsigned long
decompress_kernel(unsigned long output_start, unsigned long free_mem_ptr_p,
		unsigned long free_mem_ptr_end_p,
		int arch_id)
{
	unsigned char *tmp;
        unsigned int  size=0;
        char str[10];

	output_data		= (unsigned char *)output_start;
	free_mem_ptr		= free_mem_ptr_p;
	free_mem_end_ptr	= free_mem_ptr_end_p;
	__machine_arch_type	= arch_id;

	arch_decomp_setup();

	tmp = (unsigned char *) (((unsigned long)input_data_end) - 4);
	output_ptr = get_unaligned_le32(tmp);

        /* Check Integrity of Kernel image */
        size = input_data_end - input_data;
        putstr(itoa(size, str, 10));
        putstr("\n");
        putstr(itoa(input_data[size], str, 16));
        putstr("\n");
        putstr(itoa(input_data[size-1], str, 16));
        putstr("\n");
        putstr(itoa(input_data[size-2], str, 16));
        putstr("\n");
        putstr(itoa(input_data[size-3], str, 16));
        putstr("\n");
        putstr(itoa(input_data[size-4], str, 16));
        putstr("\n");
        putstr(itoa(input_data[size-5], str, 16));
        putstr("\n");
        if (input_data[size] == 0x31)
          putstr("1 YES\n");
        else
          putstr("1 NO\n");
        if (input_data[size-1] == 0x31)
          putstr("2 YES\n");
        else
          putstr("2 NO\n");
        if (input_data[size-2] == 0x31)
          putstr("3 YES\n");
        else
          putstr("3 NO\n");
        if (input_data[size-3] == 0x31)
          putstr("4 YES\n");
        else
          putstr("4 NO\n");

	putstr("Uncompressing Linux...");
	do_decompress(input_data, input_data_end - input_data,
			output_data, error);
	putstr(" done, booting the kernel.\n");
	return output_ptr;
}

/*
 * Written by J.T. Conklin <jtc@netbsd.org>.
 * Public domain.
 */

#include <machine/asm.h>

RCSID("$NetBSD: e_sqrtf.S,v 1.2 1995/05/08 23:50:14 jtc Exp $")

ENTRY(__ieee754_sqrtf)
	flds	4(%esp)
	fsqrt
	ret
END (__ieee754_sqrtf)

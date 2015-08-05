/*
 * Copyright (C) 2004, 2007-2010, 2011-2012 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Vineetg: August 2010: From Android kernel work
 */

#ifndef _ASM_FUTEX_H
#define _ASM_FUTEX_H

#include <linux/futex.h>
#include <linux/preempt.h>
#include <linux/uaccess.h>
#include <asm/errno.h>

#ifdef CONFIG_ARC_HAS_LLSC

#define __futex_atomic_op(insn, ret, oldval, uaddr, oparg)\
							\
	smp_mb();					\
	__asm__ __volatile__(				\
	"1:	llock	%1, [%2]		\n"	\
		insn				"\n"	\
	"2:	scond	%0, [%2]		\n"	\
	"	bnz	1b			\n"	\
	"	mov %0, 0			\n"	\
	"3:					\n"	\
	"	.section .fixup,\"ax\"		\n"	\
	"	.align  4			\n"	\
	"4:	mov %0, %4			\n"	\
	"	b   3b				\n"	\
	"	.previous			\n"	\
	"	.section __ex_table,\"a\"	\n"	\
	"	.align  4			\n"	\
	"	.word   1b, 4b			\n"	\
	"	.word   2b, 4b			\n"	\
	"	.previous			\n"	\
							\
	: "=&r" (ret), "=&r" (oldval)			\
	: "r" (uaddr), "r" (oparg), "ir" (-EFAULT)	\
	: "cc", "memory");				\
	smp_mb()					\

#else	/* !CONFIG_ARC_HAS_LLSC */

#define __futex_atomic_op(insn, ret, oldval, uaddr, oparg)\
							\
	smp_mb();					\
	__asm__ __volatile__(				\
	"1:	ld	%1, [%2]		\n"	\
		insn				"\n"	\
	"2:	st	%0, [%2]		\n"	\
	"	mov %0, 0			\n"	\
	"3:					\n"	\
	"	.section .fixup,\"ax\"		\n"	\
	"	.align  4			\n"	\
	"4:	mov %0, %4			\n"	\
	"	b   3b				\n"	\
	"	.previous			\n"	\
	"	.section __ex_table,\"a\"	\n"	\
	"	.align  4			\n"	\
	"	.word   1b, 4b			\n"	\
	"	.word   2b, 4b			\n"	\
	"	.previous			\n"	\
							\
	: "=&r" (ret), "=&r" (oldval)			\
	: "r" (uaddr), "r" (oparg), "ir" (-EFAULT)	\
	: "cc", "memory");				\
	smp_mb()					\

#endif

static inline int futex_atomic_op_inuser(int encoded_op, u32 __user *uaddr)
{
	int op = (encoded_op >> 28) & 7;
	int cmp = (encoded_op >> 24) & 15;
	int oparg = (encoded_op << 8) >> 20;
	int cmparg = (encoded_op << 20) >> 20;
	int oldval = 0, ret;

	if (encoded_op & (FUTEX_OP_OPARG_SHIFT << 28))
		oparg = 1 << oparg;

	if (!access_ok(VERIFY_WRITE, uaddr, sizeof(int)))
		return -EFAULT;

	pagefault_disable();

	switch (op) {
	case FUTEX_OP_SET:
		__futex_atomic_op("mov %0, %3", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_ADD:
		/* oldval = *uaddr; *uaddr += oparg ; ret = *uaddr */
		__futex_atomic_op("add %0, %1, %3", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_OR:
		__futex_atomic_op("or  %0, %1, %3", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_ANDN:
		__futex_atomic_op("bic %0, %1, %3", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_XOR:
		__futex_atomic_op("xor %0, %1, %3", ret, oldval, uaddr, oparg);
		break;
	default:
		ret = -ENOSYS;
	}

	pagefault_enable();

	if (!ret) {
		switch (cmp) {
		case FUTEX_OP_CMP_EQ:
			ret = (oldval == cmparg);
			break;
		case FUTEX_OP_CMP_NE:
			ret = (oldval != cmparg);
			break;
		case FUTEX_OP_CMP_LT:
			ret = (oldval < cmparg);
			break;
		case FUTEX_OP_CMP_GE:
			ret = (oldval >= cmparg);
			break;
		case FUTEX_OP_CMP_LE:
			ret = (oldval <= cmparg);
			break;
		case FUTEX_OP_CMP_GT:
			ret = (oldval > cmparg);
			break;
		default:
			ret = -ENOSYS;
		}
	}
	return ret;
}

/*
 * cmpxchg of futex (pagefaults disabled by caller)
 */
static inline int
futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr, u32 expval,
			      u32 newval)
{
	u32 existval;

	if (!access_ok(VERIFY_WRITE, uaddr, sizeof(u32)))
		return -EFAULT;

	smp_mb();

	__asm__ __volatile__(
#ifdef CONFIG_ARC_HAS_LLSC
	"1:	llock	%0, [%3]		\n"
	"	brne	%0, %1, 3f		\n"
	"2:	scond	%2, [%3]		\n"
	"	bnz	1b			\n"
#else
	"1:	ld	%0, [%3]		\n"
	"	brne	%0, %1, 3f		\n"
	"2:	st	%2, [%3]		\n"
#endif
	"3:	\n"
	"	.section .fixup,\"ax\"	\n"
	"4:	mov %0, %4	\n"
	"	b   3b	\n"
	"	.previous	\n"
	"	.section __ex_table,\"a\"	\n"
	"	.align  4	\n"
	"	.word   1b, 4b	\n"
	"	.word   2b, 4b	\n"
	"	.previous\n"
	: "=&r"(existval)
	: "r"(expval), "r"(newval), "r"(uaddr), "ir"(-EFAULT)
	: "cc", "memory");

	smp_mb();

	*uval = existval;
	return existval;
}

#endif

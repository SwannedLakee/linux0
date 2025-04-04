/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_ATOMIC64_32_H
#define _ASM_X86_ATOMIC64_32_H

#include <linux/compiler.h>
#include <linux/types.h>
//#include <asm/cmpxchg.h>

/* An 64bit atomic type */

typedef struct {
	s64 __aligned(8) counter;
} atomic64_t;

#define ATOMIC64_INIT(val)	{ (val) }

/*
 * Read an atomic64_t non-atomically.
 *
 * This is intended to be used in cases where a subsequent atomic operation
 * will handle the torn value, and can be used to prime the first iteration
 * of unconditional try_cmpxchg() loops, e.g.:
 *
 * 	s64 val = arch_atomic64_read_nonatomic(v);
 * 	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val OP i);
 *
 * This is NOT safe to use where the value is not always checked by a
 * subsequent atomic operation, such as in conditional try_cmpxchg() loops
 * that can break before the atomic operation, e.g.:
 *
 * 	s64 val = arch_atomic64_read_nonatomic(v);
 * 	do {
 * 		if (condition(val))
 * 			break;
 * 	} while (!arch_atomic64_try_cmpxchg(v, &val, val OP i);
 */
static __always_inline s64 arch_atomic64_read_nonatomic(const atomic64_t *v)
{
	/* See comment in arch_atomic_read(). */
	return __READ_ONCE(v->counter);
}

#define __ATOMIC64_DECL(sym) void atomic64_##sym(atomic64_t *, ...)
#ifndef ATOMIC64_EXPORT
#define ATOMIC64_DECL_ONE __ATOMIC64_DECL
#else
#define ATOMIC64_DECL_ONE(sym) __ATOMIC64_DECL(sym); \
	ATOMIC64_EXPORT(atomic64_##sym)
#endif

#ifdef CONFIG_X86_CX8
#define __alternative_atomic64(f, g, out, in, clobbers...)		\
	asm volatile("call %c[func]"					\
		     : ALT_OUTPUT_SP(out) \
		     : [func] "i" (atomic64_##g##_cx8)			\
		       COMMA(in)					\
		     : clobbers)

#define ATOMIC64_DECL(sym) ATOMIC64_DECL_ONE(sym##_cx8)
#else
#define __alternative_atomic64(f, g, out, in, clobbers...)		\
	alternative_call(atomic64_##f##_386, atomic64_##g##_cx8,	\
			 X86_FEATURE_CX8, ASM_OUTPUT(out),		\
			 ASM_INPUT(in), clobbers)

#define ATOMIC64_DECL(sym) ATOMIC64_DECL_ONE(sym##_cx8); \
	ATOMIC64_DECL_ONE(sym##_386)

ATOMIC64_DECL_ONE(add_386);
ATOMIC64_DECL_ONE(sub_386);
ATOMIC64_DECL_ONE(inc_386);
ATOMIC64_DECL_ONE(dec_386);
#endif

#define alternative_atomic64(f, out, in, clobbers...) \
	__alternative_atomic64(f, f, ASM_OUTPUT(out), ASM_INPUT(in), clobbers)

ATOMIC64_DECL(read);
ATOMIC64_DECL(set);
ATOMIC64_DECL(xchg);
ATOMIC64_DECL(add_return);
ATOMIC64_DECL(sub_return);
ATOMIC64_DECL(inc_return);
ATOMIC64_DECL(dec_return);
ATOMIC64_DECL(dec_if_positive);
ATOMIC64_DECL(inc_not_zero);
ATOMIC64_DECL(add_unless);

#undef ATOMIC64_DECL
#undef ATOMIC64_DECL_ONE
#undef __ATOMIC64_DECL
#undef ATOMIC64_EXPORT

static __always_inline s64 arch_atomic64_cmpxchg(atomic64_t *v, s64 old, s64 new)
{
	return arch_cmpxchg64(&v->counter, old, new);
}
#define arch_atomic64_cmpxchg arch_atomic64_cmpxchg

static __always_inline bool arch_atomic64_try_cmpxchg(atomic64_t *v, s64 *old, s64 new)
{
	return arch_try_cmpxchg64(&v->counter, old, new);
}
#define arch_atomic64_try_cmpxchg arch_atomic64_try_cmpxchg

static __always_inline s64 arch_atomic64_xchg(atomic64_t *v, s64 n)
{
	s64 o;
	unsigned high = (unsigned)(n >> 32);
	unsigned low = (unsigned)n;
	alternative_atomic64(xchg,
			     "=&A" (o),
			     ASM_INPUT("S" (v), "b" (low), "c" (high)),
			     "memory");
	return o;
}
#define arch_atomic64_xchg arch_atomic64_xchg

static __always_inline void arch_atomic64_set(atomic64_t *v, s64 i)
{
	unsigned high = (unsigned)(i >> 32);
	unsigned low = (unsigned)i;
	alternative_atomic64(set,
			     /* no output */,
			     ASM_INPUT("S" (v), "b" (low), "c" (high)),
			     "eax", "edx", "memory");
}

static __always_inline s64 arch_atomic64_read(const atomic64_t *v)
{
	s64 r;
	alternative_atomic64(read, "=&A" (r), "c" (v), "memory");
	return r;
}

static __always_inline s64 arch_atomic64_add_return(s64 i, atomic64_t *v)
{
	alternative_atomic64(add_return,
			     ASM_OUTPUT("+A" (i), "+c" (v)),
			     /* no input */,
			     "memory");
	return i;
}
#define arch_atomic64_add_return arch_atomic64_add_return

static __always_inline s64 arch_atomic64_sub_return(s64 i, atomic64_t *v)
{
	alternative_atomic64(sub_return,
			     ASM_OUTPUT("+A" (i), "+c" (v)),
			     /* no input */,
			     "memory");
	return i;
}
#define arch_atomic64_sub_return arch_atomic64_sub_return

static __always_inline s64 arch_atomic64_inc_return(atomic64_t *v)
{
	s64 a;
	alternative_atomic64(inc_return,
			     "=&A" (a),
			     "S" (v),
			     "memory", "ecx");
	return a;
}
#define arch_atomic64_inc_return arch_atomic64_inc_return

static __always_inline s64 arch_atomic64_dec_return(atomic64_t *v)
{
	s64 a;
	alternative_atomic64(dec_return,
			     "=&A" (a),
			     "S" (v),
			     "memory", "ecx");
	return a;
}
#define arch_atomic64_dec_return arch_atomic64_dec_return

static __always_inline void arch_atomic64_add(s64 i, atomic64_t *v)
{
	__alternative_atomic64(add, add_return,
			       ASM_OUTPUT("+A" (i), "+c" (v)),
			       /* no input */,
			       "memory");
}

static __always_inline void arch_atomic64_sub(s64 i, atomic64_t *v)
{
	__alternative_atomic64(sub, sub_return,
			       ASM_OUTPUT("+A" (i), "+c" (v)),
			       /* no input */,
			       "memory");
}

static __always_inline void arch_atomic64_inc(atomic64_t *v)
{
	__alternative_atomic64(inc, inc_return,
			       /* no output */,
			       "S" (v),
			       "memory", "eax", "ecx", "edx");
}
#define arch_atomic64_inc arch_atomic64_inc

static __always_inline void arch_atomic64_dec(atomic64_t *v)
{
	__alternative_atomic64(dec, dec_return,
			       /* no output */,
			       "S" (v),
			       "memory", "eax", "ecx", "edx");
}
#define arch_atomic64_dec arch_atomic64_dec

static __always_inline int arch_atomic64_add_unless(atomic64_t *v, s64 a, s64 u)
{
	unsigned low = (unsigned)u;
	unsigned high = (unsigned)(u >> 32);
	alternative_atomic64(add_unless,
			     ASM_OUTPUT("+A" (a), "+c" (low), "+D" (high)),
			     "S" (v),
			     "memory");
	return (int)a;
}
#define arch_atomic64_add_unless arch_atomic64_add_unless

static __always_inline int arch_atomic64_inc_not_zero(atomic64_t *v)
{
	int r;
	alternative_atomic64(inc_not_zero,
			     "=&a" (r),
			     "S" (v),
			     "ecx", "edx", "memory");
	return r;
}
#define arch_atomic64_inc_not_zero arch_atomic64_inc_not_zero

static __always_inline s64 arch_atomic64_dec_if_positive(atomic64_t *v)
{
	s64 r;
	alternative_atomic64(dec_if_positive,
			     "=&A" (r),
			     "S" (v),
			     "ecx", "memory");
	return r;
}
#define arch_atomic64_dec_if_positive arch_atomic64_dec_if_positive

#undef alternative_atomic64
#undef __alternative_atomic64

static __always_inline void arch_atomic64_and(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val & i));
}

static __always_inline s64 arch_atomic64_fetch_and(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val & i));

	return val;
}
#define arch_atomic64_fetch_and arch_atomic64_fetch_and

static __always_inline void arch_atomic64_or(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val | i));
}

static __always_inline s64 arch_atomic64_fetch_or(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val | i));

	return val;
}
#define arch_atomic64_fetch_or arch_atomic64_fetch_or

static __always_inline void arch_atomic64_xor(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val ^ i));
}

static __always_inline s64 arch_atomic64_fetch_xor(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val ^ i));

	return val;
}
#define arch_atomic64_fetch_xor arch_atomic64_fetch_xor

static __always_inline s64 arch_atomic64_fetch_add(s64 i, atomic64_t *v)
{
	s64 val = arch_atomic64_read_nonatomic(v);

	do { } while (!arch_atomic64_try_cmpxchg(v, &val, val + i));

	return val;
}
#define arch_atomic64_fetch_add arch_atomic64_fetch_add

#define arch_atomic64_fetch_sub(i, v)	arch_atomic64_fetch_add(-(i), (v))

#endif /* _ASM_X86_ATOMIC64_32_H */

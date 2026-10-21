// SPDX-License-Identifier: GPL-2.0

#include <linux/bitops.h>
#include <linux/find.h>

void rust_helper___set_bit(unsigned long nr, unsigned long *addr)
{
	__set_bit(nr, addr);
}

void rust_helper___clear_bit(unsigned long nr, unsigned long *addr)
{
	__clear_bit(nr, addr);
}

void rust_helper_set_bit(unsigned long nr, volatile unsigned long *addr)
{
	set_bit(nr, addr);
}

void rust_helper_clear_bit(unsigned long nr, volatile unsigned long *addr)
{
	clear_bit(nr, addr);
}

/*
 * The rust_helper_ prefix is intentionally omitted below so that the
 * declarations in include/linux/find.h are compatible with these helpers.
 *
 * Note that the below #ifdefs mean that the helper is only created if C does
 * not provide a definition.
 */
#ifdef find_first_zero_bit
__rust_helper
unsigned long _find_first_zero_bit(const unsigned long *p, unsigned long size)
{
	return find_first_zero_bit(p, size);
}
#endif /* find_first_zero_bit */

#ifdef find_next_zero_bit
__rust_helper
unsigned long _find_next_zero_bit(const unsigned long *addr,
				  unsigned long size, unsigned long offset)
{
	return find_next_zero_bit(addr, size, offset);
}
#endif /* find_next_zero_bit */

#ifdef find_first_bit
__rust_helper
unsigned long _find_first_bit(const unsigned long *addr, unsigned long size)
{
	return find_first_bit(addr, size);
}
#endif /* find_first_bit */

#ifdef find_next_bit
__rust_helper
unsigned long _find_next_bit(const unsigned long *addr, unsigned long size,
			     unsigned long offset)
{
	return find_next_bit(addr, size, offset);
}
#endif /* find_next_bit */

/*
 * The kernel's hweight64() is a macro over the architecture's
 * __arch_hweight64(), which uses the alternatives mechanism: the generic
 * software version is patched to the popcount instruction at boot when the
 * CPU has it. A Rust count_ones() is compiled to the software sequence
 * instead, so the Rust bitmap calls this helper to be on par with the C
 * implementation.
 */
unsigned long rust_helper_hweight64(u64 w)
{
	return hweight64(w);
}

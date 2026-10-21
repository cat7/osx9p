/* 64-bit shifts gcc 3.3 calls out of line; the 10.3 kernel exports none */
#include <stdint.h>

typedef union {
	uint64_t q;
	struct { uint32_t hi, lo; } w;	/* big-endian */
} dw;

uint64_t
__lshrdi3(uint64_t a, int b)
{
	dw u;

	u.q = a;
	if (b == 0)
		return a;
	if (b >= 32) {
		u.w.lo = u.w.hi >> (b - 32);
		u.w.hi = 0;
	} else {
		u.w.lo = (u.w.lo >> b) | (u.w.hi << (32 - b));
		u.w.hi >>= b;
	}
	return u.q;
}

int64_t
__ashrdi3(int64_t a, int b)
{
	dw u;
	int32_t hi;

	u.q = a;
	if (b == 0)
		return a;
	hi = (int32_t)u.w.hi;
	if (b >= 32) {
		u.w.lo = (uint32_t)(hi >> (b - 32));
		u.w.hi = (uint32_t)(hi >> 31);
	} else {
		u.w.lo = (u.w.lo >> b) | (u.w.hi << (32 - b));
		u.w.hi = (uint32_t)(hi >> b);
	}
	return u.q;
}

int64_t
__ashldi3(int64_t a, int b)
{
	dw u;

	u.q = a;
	if (b == 0)
		return a;
	if (b >= 32) {
		u.w.hi = u.w.lo << (b - 32);
		u.w.lo = 0;
	} else {
		u.w.hi = (u.w.hi << b) | (u.w.lo >> (32 - b));
		u.w.lo <<= b;
	}
	return u.q;
}

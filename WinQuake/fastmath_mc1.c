// The MRISC32 FPU has no square root instruction and newlib's sqrtf is a slow bit-by-bit loop.
// The reciprocal square root estimate refined twice by Newton-Raphson is good to about 2 ulp.
//
// newlib's sinf and cosf reduce every argument beyond pi/4 with __ieee754_rem_pio2f, which costs
// more than the polynomial. These use the Cephes reduction (pi/4 split in three parts, exact for
// |x| < 8192) and polynomials, accurate to about 1 ulp.

#ifdef __MRISC32__

#include <math.h>

float sqrtf(float x)
{
	if (!(x > 0.0f))
		return 0.0f;

	float y;
	const float half = 0.5f * x;

	__asm__("frsqrta\t%0, %1" : "=r"(y) : "r"(x));
	y = y * (1.5f - half * y * y);
	y = y * (1.5f - half * y * y);

	return x * y;
}

#define FOUR_OVER_PI	1.27323954473516f
#define DP1				0.78515625f
#define DP2				2.4187564849853515625e-4f
#define DP3				3.77489497744594108e-8f
#define REDUCE_LIMIT	8192.0f

static inline float SinPoly(const float x)
{
	const float z = x * x;
	return ((-1.9515295891e-4f * z + 8.3321608736e-3f) * z - 1.6666654611e-1f) * z * x + x;
}

static inline float CosPoly(const float x)
{
	const float z = x * x;
	return ((2.443315711809948e-5f * z - 1.388731625493765e-3f) * z + 4.166664568298827e-2f) * z * z - 0.5f * z + 1.0f;
}

// Reduces |x| to [-pi/4, pi/4]; the result is in octant pair 0..3, flipping the sign for 4..7.
static inline float Reduce(float ax, int* octant, int* negate)
{
	if (!(ax <= REDUCE_LIMIT)) // Also NaN and infinity, which come out as NaN.
		ax = fmodf(ax, 6.283185307179586f);

	int j = (int)(FOUR_OVER_PI * ax);
	float y = (float)j;

	if (j & 1)
	{
		j++;
		y += 1.0f;
	}

	j &= 7;
	if (j > 3)
	{
		*negate = !*negate;
		j -= 4;
	}

	*octant = j;
	return ((ax - y * DP1) - y * DP2) - y * DP3;
}

float sinf(float x)
{
	const float ax = fabsf(x);

	if (ax <= 0.785398163f)
		return SinPoly(x);

	int negate = (x < 0.0f);
	int octant;
	const float r = Reduce(ax, &octant, &negate);
	const float y = (octant == 1 || octant == 2) ? CosPoly(r) : SinPoly(r);

	return negate ? -y : y;
}

float cosf(float x)
{
	const float ax = fabsf(x);

	if (ax <= 0.785398163f)
		return CosPoly(ax);

	int negate = 0;
	int octant;
	const float r = Reduce(ax, &octant, &negate);

	if (octant > 1)
		negate = !negate;

	const float y = (octant == 1 || octant == 2) ? SinPoly(r) : CosPoly(r);

	return negate ? -y : y;
}

#endif

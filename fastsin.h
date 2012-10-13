#define SIN_TABSIZ 8192 /* a power of two */
#define SIN_TABMASK ((size_t)SIN_TABSIZ-1)
#define SIN_SCALEFACTOR (SIN_TABSIZ / (2*M_PI))
#define FASTSIN_PERIODICITY (2*M_PI*SIN_SCALEFACTOR)

extern double sintable[SIN_TABSIZ];

/* Call this before using FASTSIN(). */
void initsintable(void);

/*
 * FASTSIN() is like sin() but less precise.
 * And the input has to be scaled by SIN_SCALEFACTOR first.
 */

#define FASTSIN(x) (sintable[SIN_TABMASK & (x>>FRACBITS)])

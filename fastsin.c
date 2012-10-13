#include <stddef.h>
#include <math.h>
#include "fixed.h"
#include "fastsin.h"

double sintable[SIN_TABSIZ];

void initsintable(void)
{
	int i;
	for (i = 0; i < SIN_TABSIZ; i++)
		sintable[i] = sin((double)i * 2*M_PI / SIN_TABSIZ);
}

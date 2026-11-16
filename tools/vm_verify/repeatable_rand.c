/* Preloaded by tools/vm_verify/run.py --state: rand() gives every thread the same sequence in every run and
   srand() does nothing (the engine seeds from the clock in several places), so two engines that run the same scripts
   draw the same numbers. */
#include <stdlib.h>

static __thread unsigned long long state = 0x9E3779B97F4A7C15ull;

void srand(unsigned seed)
{
    (void)seed;
}

int rand(void)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return (int)((state >> 33) & RAND_MAX);
}

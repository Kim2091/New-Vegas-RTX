#include "std_include.hpp"

// The wrapper is 32-bit only.
static_assert(sizeof(intptr_t) == 4 && sizeof(void*) == 4 && sizeof(size_t) == 4, "This doesn't seem to be a 32-bit environment!");

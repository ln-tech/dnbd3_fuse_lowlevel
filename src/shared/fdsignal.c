#include "fdsignal.h"

#if defined(linux) || defined(__linux) || defined(__linux__)
//#warning "Using eventfd based signalling"
#include "fdsignal.inc/eventfd.c"
#elif __SIZEOF_INT__ == 4 && __SIZEOF_POINTER__ == 8
//#warning "Using pointer-packing pipe based signalling"
#include "fdsignal.inc/pipe64.c"
#else
//#warning "Using fallback pipe based signalling"
#include "fdsignal.inc/pipe_malloc.c"
#endif

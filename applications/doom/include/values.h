#ifndef _VALUES_H
#define _VALUES_H

#include <limits.h>

#ifndef BITSPERBYTE
#define BITSPERBYTE 8
#endif

#ifndef MAXINT
#define MAXINT 0x7fffffff
#endif
#ifndef MININT
#define MININT (-0x7fffffff - 1)
#endif
#ifndef MAXSHORT
#define MAXSHORT 0x7fff
#endif
#ifndef MINSHORT
#define MINSHORT (-0x7fff - 1)
#endif
#ifndef MAXLONG
#define MAXLONG 0x7fffffffL
#endif
#ifndef MINLONG
#define MINLONG (-0x7fffffffL - 1)
#endif

#endif

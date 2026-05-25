/* Feature-test macros must precede every system header so the libc exposes
   the extensions the runtime uses. The whole program is one translation unit
   (prelude + runtime + user code concatenated), and prelude is emitted first,
   so defining them here covers everything downstream. sched.c's own
   `#define _GNU_SOURCE` lands after <stdio.h> already locked glibc's feature
   set, which left CPU_SET / CPU_SETSIZE / accept4 undeclared under plain gcc.
   On macOS the deprecated ucontext routines require _XOPEN_SOURCE, and
   _DARWIN_C_SOURCE re-opens the BSD surface that _XOPEN_SOURCE alone hides. */
#if defined(__APPLE__)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE 1
#  endif
#elif !defined(_WIN32)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE 1
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

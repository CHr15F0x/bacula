/*
    Integer types.  These types should be be used in all
    contexts in which the length of an integer stored on
    removable media must be known regardless of the
    architecture of the platform.

    Bacula types are:

    int8_t,  int16_t,  int32_t,  int64_t
    uint8_t, uint16_t, uint32_t, uint64_t

    Also, we define types such as file address lengths.

    Version $Id$

 */
/*
   Copyright (C) 2000-2004 Kern Sibbald and John Walker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

 */


#ifndef __bc_types_INCLUDED
#define __bc_types_INCLUDED

typedef char POOLMEM;   
#define mp_chr(x) x
#ifdef  xxxxx
#define mp_chr(x) ((char*)(x))
class POOLMEM { 
public:
   POOLMEM();
   operator char * const() { return this; }
};
#endif


/* Types */

/* If sys/types.h does not supply intXX_t, supply them ourselves */
/* (or die trying) */

#ifndef HAVE_U_INT
typedef unsigned int u_int;
#endif

#ifndef HAVE_INTXX_T
# if (SIZEOF_CHAR == 1)
typedef char int8_t;
# else
#  error "8 bit int type not found."
# endif
# if (SIZEOF_SHORT_INT == 2)
typedef short int int16_t;
# else
#  error "16 bit int type not found."
# endif
# if (SIZEOF_INT == 4)
typedef int int32_t;
# else
#  error "32 bit int type not found."
# endif
#endif

/* If sys/types.h does not supply u_intXX_t, supply them ourselves */
#ifndef HAVE_U_INTXX_T
# ifdef HAVE_UINTXX_T
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
# define HAVE_U_INTXX_T 1
# else
#  if (SIZEOF_CHAR == 1)
typedef unsigned char u_int8_t;
#  else
#   error "8 bit int type not found. Required!"
#  endif
#  if (SIZEOF_SHORT_INT == 2)
typedef unsigned short int u_int16_t;
#  else
#   error "16 bit int type not found. Required!"
#  endif
#  if (SIZEOF_INT == 4)
typedef unsigned int u_int32_t;
#  else
#   error "32 bit int type not found. Required!"
#  endif
# endif
#endif

/* 64-bit types */
#ifndef HAVE_INT64_T
# if (SIZEOF_LONG_LONG_INT == 8)
typedef long long int int64_t;
#   define HAVE_INT64_T 1
# else
#  if (SIZEOF_LONG_INT == 8)
typedef long int int64_t;
#   define HAVE_INT64_T 1
#  endif
# endif
#endif

#ifndef HAVE_INTMAX_T
# ifdef HAVE_INT64_T
typedef int64_t intmax_t;
# else
#   error "64 bit type not found. Required!"
# endif
#endif

#ifndef HAVE_U_INT64_T
# if (SIZEOF_LONG_LONG_INT == 8)
typedef unsigned long long int u_int64_t;
#   define HAVE_U_INT64_T 1
# else
#  if (SIZEOF_LONG_INT == 8)
typedef unsigned long int u_int64_t;
#   define HAVE_U_INT64_T 1
#  else
#   error "64 bit type not found. Required!"
#  endif
# endif
#endif

#ifndef HAVE_U_INTMAX_T
# ifdef HAVE_U_INT64_T
typedef u_int64_t u_intmax_t;
# else
#   error "64 bit type not found. Required!"
# endif
#endif


/* Limits for the above types. */
#undef INT8_MIN  
#undef INT8_MAX  
#undef UINT8_MAX 
#undef INT16_MIN 
#undef INT16_MAX 
#undef UINT16_MAX
#undef INT32_MIN 
#undef INT32_MAX 
#undef UINT32_MAX

#define INT8_MIN        (-127-1)
#define INT8_MAX        (127)
#define UINT8_MAX       (255u)
#define INT16_MIN       (-32767-1)
#define INT16_MAX       (32767)
#define UINT16_MAX      (65535u)
#define INT32_MIN       (-2147483647-1)
#define INT32_MAX       (2147483647)
#define UINT32_MAX      (4294967295u)

typedef double            float64_t;
typedef float             float32_t;

#endif /* __bc_types_INCLUDED */

/* Define the uint versions actually used in Bacula */
#define uint8_t u_int8_t
#define uint16_t u_int16_t
#define uint32_t u_int32_t
#define uint64_t u_int64_t
#define uintmax_t u_intmax_t

/* Bacula time -- Unix time with microseconds */
#define btime_t int64_t
/* Unix time (time_t) widened to 64 bits */
#define utime_t int64_t

#ifdef HAVE_CYGWIN
#define int_least16_t int32_t
#endif

#ifndef HAVE_SOCKLEN_T
#define socklen_t int
#endif

#ifdef HAVE_OLD_SOCKOPT
#define sockopt_val_t char *
#else
#define sockopt_val_t void *
#endif

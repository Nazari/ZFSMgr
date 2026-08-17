/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 2

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1

/* Substitute the type names.  */
#define YYSTYPE         ZFSMCLISTYPE
/* Substitute the variable and function names.  */
#define yyparse         zfsmcliparse
#define yylex           zfsmclilex
#define yyerror         zfsmclierror
#define yydebug         zfsmclidebug
#define yynerrs         zfsmclinerrs

/* First part of user prologue.  */
#line 1 "gramatica.y"

/* Gramática LALR(1) de la línea del intérprete.
 *
 * Por qué existe, con franqueza: lo que había antes resolvía las ambigüedades con reglas
 * de precedencia escritas a mano —«primero mira si nombra un pool, luego si lo quiere la
 * ranura, luego si vale como destino»—, y esas reglas se descubrían EJECUTANDO órdenes y
 * viendo salidas raras. `get compression` preguntaba por el dataset
 * `tank/datos/compression`; `trim <pool> <disco>` mandaba el disco donde iba el pool.
 * Ninguno de los dos fallaba al compilar.
 *
 * Aquí, una ambigüedad es un CONFLICTO que bison reporta al construir. Es la diferencia
 * entre una regla que hay que recordar y una que se comprueba sola: `%expect 0` hace que
 * el build falle si alguien introduce una.
 *
 * **Se apoya en dos decisiones que hacen la gramática posible:**
 *
 *  1. Una URL y una palabra son componentes distintos, y se distinguen por su FORMA
 *     (`gramatica.l`). Por eso `flush /local/tank` es un destino y `scrub stop` no.
 *  2. El verbo se clasifica por su FORMA —qué objetivo y qué ranuras admite— leyéndolo del
 *     catálogo de `ayuda.cpp`. Así la gramática tiene una producción por forma y no una por
 *     verbo: añadir una orden es una fila en una tabla, no tocar esto.
 *
 * Ver docs/gramatica_cli.md.
 */
#include <stdio.h>
#include <string.h>

#line 105 "generado/gramatica.tab.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "gramatica.tab.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_URL = 3,                        /* URL  */
  YYSYMBOL_PALABRA = 4,                    /* PALABRA  */
  YYSYMBOL_ASIGNACION = 5,                 /* ASIGNACION  */
  YYSYMBOL_OPCION_LARGA = 6,               /* OPCION_LARGA  */
  YYSYMBOL_OPCION_CORTA = 7,               /* OPCION_CORTA  */
  YYSYMBOL_FASE_START = 8,                 /* FASE_START  */
  YYSYMBOL_FASE_STOP = 9,                  /* FASE_STOP  */
  YYSYMBOL_FASE_CANCEL = 10,               /* FASE_CANCEL  */
  YYSYMBOL_FASE_PAUSE = 11,                /* FASE_PAUSE  */
  YYSYMBOL_FASE_SUSPEND = 12,              /* FASE_SUSPEND  */
  YYSYMBOL_CARACTER_MALO = 13,             /* CARACTER_MALO  */
  YYSYMBOL_V_NADA = 14,                    /* V_NADA  */
  YYSYMBOL_V_TEXTO = 15,                   /* V_TEXTO  */
  YYSYMBOL_V_CUALQUIERA = 16,              /* V_CUALQUIERA  */
  YYSYMBOL_V_CONN = 17,                    /* V_CONN  */
  YYSYMBOL_V_CONN_TEXTO = 18,              /* V_CONN_TEXTO  */
  YYSYMBOL_V_POOL = 19,                    /* V_POOL  */
  YYSYMBOL_V_POOL_FASE = 20,               /* V_POOL_FASE  */
  YYSYMBOL_V_POOL_FASE_VDEV = 21,          /* V_POOL_FASE_VDEV  */
  YYSYMBOL_V_POOL_VDEV = 22,               /* V_POOL_VDEV  */
  YYSYMBOL_V_DS = 23,                      /* V_DS  */
  YYSYMBOL_V_DS_TEXTO_OPC = 24,            /* V_DS_TEXTO_OPC  */
  YYSYMBOL_V_DS_ASIGNA = 25,               /* V_DS_ASIGNA  */
  YYSYMBOL_V_DS_URL = 26,                  /* V_DS_URL  */
  YYSYMBOL_V_DS_RUTA = 27,                 /* V_DS_RUTA  */
  YYSYMBOL_V_DS_TEXTO_MAS = 28,            /* V_DS_TEXTO_MAS  */
  YYSYMBOL_V_SNAP = 29,                    /* V_SNAP  */
  YYSYMBOL_V_SNAP_TEXTO = 30,              /* V_SNAP_TEXTO  */
  YYSYMBOL_V_SNAP_URL = 31,                /* V_SNAP_URL  */
  YYSYMBOL_YYACCEPT = 32,                  /* $accept  */
  YYSYMBOL_linea = 33,                     /* linea  */
  YYSYMBOL_orden = 34,                     /* orden  */
  YYSYMBOL_url_opt = 35,                   /* url_opt  */
  YYSYMBOL_conexion_opt = 36,              /* conexion_opt  */
  YYSYMBOL_fase_opt = 37,                  /* fase_opt  */
  YYSYMBOL_vdev_opt = 38,                  /* vdev_opt  */
  YYSYMBOL_texto_opt = 39,                 /* texto_opt  */
  YYSYMBOL_textos = 40,                    /* textos  */
  YYSYMBOL_componente_texto = 41,          /* componente_texto  */
  YYSYMBOL_ruta = 42,                      /* ruta  */
  YYSYMBOL_asignaciones = 43,              /* asignaciones  */
  YYSYMBOL_palabra = 44,                   /* palabra  */
  YYSYMBOL_opciones = 45,                  /* opciones  */
  YYSYMBOL_valor_opcion = 46               /* valor_opcion  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined ZFSMCLISTYPE_IS_TRIVIAL && ZFSMCLISTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  61
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   69

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  32
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  15
/* YYNRULES -- Number of rules.  */
#define YYNRULES  59
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  78

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   286


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31
};

#if ZFSMCLIDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint8 yyrline[] =
{
       0,    81,    81,    82,    89,    90,    91,    92,    95,    96,
      97,    98,    99,   100,   101,   102,   107,   108,   109,   110,
     111,   112,   116,   117,   123,   124,   125,   129,   130,   131,
     132,   133,   134,   138,   139,   140,   144,   145,   152,   153,
     157,   158,   159,   163,   164,   168,   169,   175,   176,   177,
     178,   179,   180,   183,   184,   185,   186,   190,   191,   192
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if ZFSMCLIDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "URL", "PALABRA",
  "ASIGNACION", "OPCION_LARGA", "OPCION_CORTA", "FASE_START", "FASE_STOP",
  "FASE_CANCEL", "FASE_PAUSE", "FASE_SUSPEND", "CARACTER_MALO", "V_NADA",
  "V_TEXTO", "V_CUALQUIERA", "V_CONN", "V_CONN_TEXTO", "V_POOL",
  "V_POOL_FASE", "V_POOL_FASE_VDEV", "V_POOL_VDEV", "V_DS",
  "V_DS_TEXTO_OPC", "V_DS_ASIGNA", "V_DS_URL", "V_DS_RUTA",
  "V_DS_TEXTO_MAS", "V_SNAP", "V_SNAP_TEXTO", "V_SNAP_URL", "$accept",
  "linea", "orden", "url_opt", "conexion_opt", "fase_opt", "vdev_opt",
  "texto_opt", "textos", "componente_texto", "ruta", "asignaciones",
  "palabra", "opciones", "valor_opcion", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-16)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int8 yypact[] =
{
     -13,   -16,    34,    36,    20,    36,    36,    36,    39,    52,
      36,    36,    36,    38,    54,    25,    36,    36,    58,    62,
     -16,   -16,   -16,   -16,   -16,   -16,   -16,   -16,   -16,   -16,
     -16,   -16,   -16,   -16,    34,   -16,    39,   -16,   -16,   -16,
     -16,   -16,    52,   -16,   -16,   -16,   -16,    34,    59,   -16,
     -16,   -16,   -16,   -16,   -16,    25,   -16,   -16,   -16,    61,
     -16,   -16,    53,   -16,   -16,   -16,   -16,   -16,    63,   -16,
     -16,    49,   -16,   -16,   -16,   -16,   -16,   -16
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       2,     4,    36,    22,    24,    22,    22,    22,    27,    33,
      22,    22,    22,     0,     0,     0,    22,    22,     0,     0,
      53,    47,    48,    49,    50,    51,    52,     5,    37,    23,
       6,    25,    26,     7,     0,     9,    27,    28,    29,    30,
      31,    32,    33,    35,    34,    17,    11,    36,     0,    18,
      43,    44,    19,    41,    42,    20,    38,    40,    14,     0,
      21,     1,     3,     8,    10,    16,    12,    45,    13,    39,
      15,    54,    56,    46,    58,    57,    59,    55
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -16,   -16,   -16,    15,   -16,    27,    24,    22,   -16,    12,
     -16,   -16,   -15,   -16,   -16
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
       0,    19,    20,    30,    33,    42,    45,    27,    55,    56,
      52,    68,    28,    62,    77
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int8 yytable[] =
{
      57,     1,     2,     3,     4,     5,     6,     7,     8,     9,
      10,    11,    12,    13,    14,    15,    16,    17,    18,    63,
      34,    35,    36,    31,    32,    46,    47,    48,    53,    21,
      54,    58,    59,    22,    23,    24,    25,    26,    21,    29,
      57,    49,    22,    23,    24,    25,    26,    37,    38,    39,
      40,    41,    74,    75,    76,    43,    44,    50,    51,    71,
      72,    60,    61,    64,    67,    70,    65,    69,    73,    66
};

static const yytype_int8 yycheck[] =
{
      15,    14,    15,    16,    17,    18,    19,    20,    21,    22,
      23,    24,    25,    26,    27,    28,    29,    30,    31,    34,
       5,     6,     7,     3,     4,    10,    11,    12,     3,     4,
       5,    16,    17,     8,     9,    10,    11,    12,     4,     3,
      55,     3,     8,     9,    10,    11,    12,     8,     9,    10,
      11,    12,     3,     4,     5,     3,     4,     3,     4,     6,
       7,     3,     0,    36,     5,     4,    42,    55,     5,    47
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    14,    15,    16,    17,    18,    19,    20,    21,    22,
      23,    24,    25,    26,    27,    28,    29,    30,    31,    33,
      34,     4,     8,     9,    10,    11,    12,    39,    44,     3,
      35,     3,     4,    36,    35,    35,    35,     8,     9,    10,
      11,    12,    37,     3,     4,    38,    35,    35,    35,     3,
       3,     4,    42,     3,     5,    40,    41,    44,    35,    35,
       3,     0,    45,    44,    37,    38,    39,     5,    43,    41,
       4,     6,     7,     5,     3,     4,     5,    46
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    32,    33,    33,    34,    34,    34,    34,    34,    34,
      34,    34,    34,    34,    34,    34,    34,    34,    34,    34,
      34,    34,    35,    35,    36,    36,    36,    37,    37,    37,
      37,    37,    37,    38,    38,    38,    39,    39,    40,    40,
      41,    41,    41,    42,    42,    43,    43,    44,    44,    44,
      44,    44,    44,    45,    45,    45,    45,    46,    46,    46
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     0,     2,     1,     2,     2,     2,     3,     2,
       3,     2,     3,     3,     2,     3,     3,     2,     2,     2,
       2,     2,     0,     1,     0,     1,     1,     0,     1,     1,
       1,     1,     1,     0,     1,     1,     0,     1,     1,     2,
       1,     1,     1,     1,     1,     1,     2,     1,     1,     1,
       1,     1,     1,     0,     2,     3,     2,     1,     1,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = ZFSMCLIEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == ZFSMCLIEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (scanner, res, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use ZFSMCLIerror or ZFSMCLIUNDEF. */
#define YYERRCODE ZFSMCLIUNDEF


/* Enable debugging if requested.  */
#if ZFSMCLIDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, scanner, res); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, void* scanner, AnalisisCli* res)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (scanner);
  YY_USE (res);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, void* scanner, AnalisisCli* res)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep, scanner, res);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule, void* scanner, AnalisisCli* res)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)], scanner, res);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule, scanner, res); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !ZFSMCLIDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !ZFSMCLIDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, void* scanner, AnalisisCli* res)
{
  YY_USE (yyvaluep);
  YY_USE (scanner);
  YY_USE (res);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  switch (yykind)
    {
    case YYSYMBOL_URL: /* URL  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 929 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_PALABRA: /* PALABRA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 935 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_ASIGNACION: /* ASIGNACION  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 941 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_OPCION_LARGA: /* OPCION_LARGA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 947 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_OPCION_CORTA: /* OPCION_CORTA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 953 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_START: /* FASE_START  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 959 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_STOP: /* FASE_STOP  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 965 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_CANCEL: /* FASE_CANCEL  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 971 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_PAUSE: /* FASE_PAUSE  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 977 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_SUSPEND: /* FASE_SUSPEND  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 983 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_NADA: /* V_NADA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 989 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_TEXTO: /* V_TEXTO  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 995 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CUALQUIERA: /* V_CUALQUIERA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1001 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CONN: /* V_CONN  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1007 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CONN_TEXTO: /* V_CONN_TEXTO  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1013 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_POOL: /* V_POOL  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1019 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_POOL_FASE: /* V_POOL_FASE  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1025 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_POOL_FASE_VDEV: /* V_POOL_FASE_VDEV  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1031 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_POOL_VDEV: /* V_POOL_VDEV  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1037 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DS: /* V_DS  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1043 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DS_TEXTO_OPC: /* V_DS_TEXTO_OPC  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1049 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DS_ASIGNA: /* V_DS_ASIGNA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1055 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DS_URL: /* V_DS_URL  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1061 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DS_RUTA: /* V_DS_RUTA  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1067 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DS_TEXTO_MAS: /* V_DS_TEXTO_MAS  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1073 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SNAP: /* V_SNAP  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1079 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SNAP_TEXTO: /* V_SNAP_TEXTO  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1085 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SNAP_URL: /* V_SNAP_URL  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1091 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_componente_texto: /* componente_texto  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1097 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_palabra: /* palabra  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1103 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_valor_opcion: /* valor_opcion  */
#line 76 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1109 "generado/gramatica.tab.c"
        break;

      default:
        break;
    }
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (void* scanner, AnalisisCli* res)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = ZFSMCLIEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == ZFSMCLIEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval, scanner);
    }

  if (yychar <= ZFSMCLIEOF)
    {
      yychar = ZFSMCLIEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == ZFSMCLIerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = ZFSMCLIUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = ZFSMCLIEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* linea: %empty  */
#line 81 "gramatica.y"
                                         { res->vacia = 1; }
#line 1385 "generado/gramatica.tab.c"
    break;

  case 4: /* orden: V_NADA  */
#line 89 "gramatica.y"
                                               { astVerbo(res, (yyvsp[0].texto)); }
#line 1391 "generado/gramatica.tab.c"
    break;

  case 5: /* orden: V_TEXTO texto_opt  */
#line 90 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1397 "generado/gramatica.tab.c"
    break;

  case 6: /* orden: V_CUALQUIERA url_opt  */
#line 91 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1403 "generado/gramatica.tab.c"
    break;

  case 7: /* orden: V_CONN conexion_opt  */
#line 92 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1409 "generado/gramatica.tab.c"
    break;

  case 8: /* orden: V_CONN_TEXTO url_opt palabra  */
#line 95 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1415 "generado/gramatica.tab.c"
    break;

  case 9: /* orden: V_POOL url_opt  */
#line 96 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1421 "generado/gramatica.tab.c"
    break;

  case 10: /* orden: V_POOL_FASE url_opt fase_opt  */
#line 97 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-2].texto)); }
#line 1427 "generado/gramatica.tab.c"
    break;

  case 11: /* orden: V_DS url_opt  */
#line 98 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1433 "generado/gramatica.tab.c"
    break;

  case 12: /* orden: V_DS_TEXTO_OPC url_opt texto_opt  */
#line 99 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-2].texto)); }
#line 1439 "generado/gramatica.tab.c"
    break;

  case 13: /* orden: V_DS_ASIGNA url_opt asignaciones  */
#line 100 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-2].texto)); }
#line 1445 "generado/gramatica.tab.c"
    break;

  case 14: /* orden: V_SNAP url_opt  */
#line 101 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1451 "generado/gramatica.tab.c"
    break;

  case 15: /* orden: V_SNAP_TEXTO url_opt PALABRA  */
#line 102 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "etiqueta", (yyvsp[0].texto)); }
#line 1457 "generado/gramatica.tab.c"
    break;

  case 16: /* orden: V_POOL_FASE_VDEV fase_opt vdev_opt  */
#line 107 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-2].texto)); }
#line 1463 "generado/gramatica.tab.c"
    break;

  case 17: /* orden: V_POOL_VDEV vdev_opt  */
#line 108 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1469 "generado/gramatica.tab.c"
    break;

  case 18: /* orden: V_DS_URL URL  */
#line 109 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "destino", (yyvsp[0].texto)); }
#line 1475 "generado/gramatica.tab.c"
    break;

  case 19: /* orden: V_DS_RUTA ruta  */
#line 110 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1481 "generado/gramatica.tab.c"
    break;

  case 20: /* orden: V_DS_TEXTO_MAS textos  */
#line 111 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); }
#line 1487 "generado/gramatica.tab.c"
    break;

  case 21: /* orden: V_SNAP_URL URL  */
#line 112 "gramatica.y"
                                               { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "destino", (yyvsp[0].texto)); }
#line 1493 "generado/gramatica.tab.c"
    break;

  case 23: /* url_opt: URL  */
#line 117 "gramatica.y"
                                               { astObjetivo(res, (yyvsp[0].texto)); }
#line 1499 "generado/gramatica.tab.c"
    break;

  case 25: /* conexion_opt: URL  */
#line 124 "gramatica.y"
                                               { astObjetivo(res, (yyvsp[0].texto)); }
#line 1505 "generado/gramatica.tab.c"
    break;

  case 26: /* conexion_opt: PALABRA  */
#line 125 "gramatica.y"
                                               { astObjetivo(res, (yyvsp[0].texto)); }
#line 1511 "generado/gramatica.tab.c"
    break;

  case 28: /* fase_opt: FASE_START  */
#line 130 "gramatica.y"
                                               { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 1517 "generado/gramatica.tab.c"
    break;

  case 29: /* fase_opt: FASE_STOP  */
#line 131 "gramatica.y"
                                               { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 1523 "generado/gramatica.tab.c"
    break;

  case 30: /* fase_opt: FASE_CANCEL  */
#line 132 "gramatica.y"
                                               { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 1529 "generado/gramatica.tab.c"
    break;

  case 31: /* fase_opt: FASE_PAUSE  */
#line 133 "gramatica.y"
                                               { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 1535 "generado/gramatica.tab.c"
    break;

  case 32: /* fase_opt: FASE_SUSPEND  */
#line 134 "gramatica.y"
                                               { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 1541 "generado/gramatica.tab.c"
    break;

  case 34: /* vdev_opt: PALABRA  */
#line 139 "gramatica.y"
                                               { astRanura(res, "disco", (yyvsp[0].texto)); }
#line 1547 "generado/gramatica.tab.c"
    break;

  case 35: /* vdev_opt: URL  */
#line 140 "gramatica.y"
                                               { astRanura(res, "disco", (yyvsp[0].texto)); }
#line 1553 "generado/gramatica.tab.c"
    break;

  case 37: /* texto_opt: palabra  */
#line 145 "gramatica.y"
                                               { astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1559 "generado/gramatica.tab.c"
    break;

  case 38: /* textos: componente_texto  */
#line 152 "gramatica.y"
                                               { astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1565 "generado/gramatica.tab.c"
    break;

  case 39: /* textos: textos componente_texto  */
#line 153 "gramatica.y"
                                               { astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1571 "generado/gramatica.tab.c"
    break;

  case 40: /* componente_texto: palabra  */
#line 157 "gramatica.y"
                   { (yyval.texto) = (yyvsp[0].texto); }
#line 1577 "generado/gramatica.tab.c"
    break;

  case 41: /* componente_texto: URL  */
#line 158 "gramatica.y"
                   { (yyval.texto) = (yyvsp[0].texto); }
#line 1583 "generado/gramatica.tab.c"
    break;

  case 42: /* componente_texto: ASIGNACION  */
#line 159 "gramatica.y"
                   { (yyval.texto) = (yyvsp[0].texto); }
#line 1589 "generado/gramatica.tab.c"
    break;

  case 43: /* ruta: URL  */
#line 163 "gramatica.y"
                                               { astRanura(res, "ruta", (yyvsp[0].texto)); }
#line 1595 "generado/gramatica.tab.c"
    break;

  case 44: /* ruta: PALABRA  */
#line 164 "gramatica.y"
                                               { astRanura(res, "ruta", (yyvsp[0].texto)); }
#line 1601 "generado/gramatica.tab.c"
    break;

  case 45: /* asignaciones: ASIGNACION  */
#line 168 "gramatica.y"
                                               { astRanura(res, "props", (yyvsp[0].texto)); }
#line 1607 "generado/gramatica.tab.c"
    break;

  case 46: /* asignaciones: asignaciones ASIGNACION  */
#line 169 "gramatica.y"
                                               { astRanura(res, "props", (yyvsp[0].texto)); }
#line 1613 "generado/gramatica.tab.c"
    break;

  case 47: /* palabra: PALABRA  */
#line 175 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1619 "generado/gramatica.tab.c"
    break;

  case 48: /* palabra: FASE_START  */
#line 176 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1625 "generado/gramatica.tab.c"
    break;

  case 49: /* palabra: FASE_STOP  */
#line 177 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1631 "generado/gramatica.tab.c"
    break;

  case 50: /* palabra: FASE_CANCEL  */
#line 178 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1637 "generado/gramatica.tab.c"
    break;

  case 51: /* palabra: FASE_PAUSE  */
#line 179 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1643 "generado/gramatica.tab.c"
    break;

  case 52: /* palabra: FASE_SUSPEND  */
#line 180 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1649 "generado/gramatica.tab.c"
    break;

  case 54: /* opciones: opciones OPCION_LARGA  */
#line 184 "gramatica.y"
                                               { astOpcion(res, (yyvsp[0].texto), 0); }
#line 1655 "generado/gramatica.tab.c"
    break;

  case 55: /* opciones: opciones OPCION_LARGA valor_opcion  */
#line 185 "gramatica.y"
                                               { astOpcion(res, (yyvsp[-1].texto), (yyvsp[0].texto)); }
#line 1661 "generado/gramatica.tab.c"
    break;

  case 56: /* opciones: opciones OPCION_CORTA  */
#line 186 "gramatica.y"
                                               { astBandera(res, (yyvsp[0].texto)); }
#line 1667 "generado/gramatica.tab.c"
    break;

  case 57: /* valor_opcion: PALABRA  */
#line 190 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1673 "generado/gramatica.tab.c"
    break;

  case 58: /* valor_opcion: URL  */
#line 191 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1679 "generado/gramatica.tab.c"
    break;

  case 59: /* valor_opcion: ASIGNACION  */
#line 192 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 1685 "generado/gramatica.tab.c"
    break;


#line 1689 "generado/gramatica.tab.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == ZFSMCLIEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (scanner, res, YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= ZFSMCLIEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == ZFSMCLIEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, scanner, res);
          yychar = ZFSMCLIEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, scanner, res);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (scanner, res, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != ZFSMCLIEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, scanner, res);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, scanner, res);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 195 "gramatica.y"


void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg) {
    (void)scanner;
    astError(res, msg);
}

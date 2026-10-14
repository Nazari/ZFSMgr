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
 * Por qué existe: lo que había antes resolvía las ambigüedades con reglas de precedencia
 * escritas a mano —«primero mira si nombra un pool, luego si lo quiere la ranura, luego si
 * vale como destino»— y esas reglas se descubrían EJECUTANDO órdenes y viendo salidas
 * raras. `get compression` preguntaba por el dataset `tank/datos/compression`;
 * `trim <pool> <disco>` mandaba el disco donde iba el pool. Ninguno fallaba al compilar.
 *
 * Aquí una ambigüedad es un CONFLICTO que bison reporta al construir, con el caso concreto
 * delante. `%expect 0` hace que el build falle si alguien introduce una.
 *
 * **UNA PRODUCCIÓN POR ORDEN, no por «forma» compartida.** Es más largo, y es a propósito:
 * así la gramática se lee como la lista de lo que se puede teclear, y cambiar la sintaxis
 * de una orden es editar SU regla. Con formas compartidas había que averiguar primero qué
 * otras órdenes usaban la misma y decidir si se partía en dos, que es justo la fricción
 * que uno no quiere al tocar la sintaxis.
 *
 * **Lo que hace posible la gramática está en el léxico**: una URL y una palabra son
 * componentes DISTINTOS y se distinguen por su forma. Por eso `scrub stop` y
 * `scrub /local/tank` no son la misma frase, y no hace falta preguntarle al daemon qué
 * pools existen para saberlo.
 *
 * Ver docs/gramatica_cli.md.
 */
#include <stdio.h>
#include <string.h>

#line 106 "generado/gramatica.tab.c"

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
  YYSYMBOL_FASE_START = 6,                 /* FASE_START  */
  YYSYMBOL_FASE_STOP = 7,                  /* FASE_STOP  */
  YYSYMBOL_FASE_CANCEL = 8,                /* FASE_CANCEL  */
  YYSYMBOL_FASE_PAUSE = 9,                 /* FASE_PAUSE  */
  YYSYMBOL_FASE_SUSPEND = 10,              /* FASE_SUSPEND  */
  YYSYMBOL_CARACTER_MALO = 11,             /* CARACTER_MALO  */
  YYSYMBOL_V_CD = 12,                      /* V_CD  */
  YYSYMBOL_V_LS = 13,                      /* V_LS  */
  YYSYMBOL_V_PWD = 14,                     /* V_PWD  */
  YYSYMBOL_V_INFO = 15,                    /* V_INFO  */
  YYSYMBOL_V_HELP = 16,                    /* V_HELP  */
  YYSYMBOL_V_EXIT = 17,                    /* V_EXIT  */
  YYSYMBOL_V_FORMAT = 18,                  /* V_FORMAT  */
  YYSYMBOL_V_YES = 19,                     /* V_YES  */
  YYSYMBOL_V_CLS = 20,                     /* V_CLS  */
  YYSYMBOL_V_MASTER_PASSWORD = 21,         /* V_MASTER_PASSWORD  */
  YYSYMBOL_V_CONNECT = 22,                 /* V_CONNECT  */
  YYSYMBOL_V_DISCONNECT = 23,              /* V_DISCONNECT  */
  YYSYMBOL_V_REFRESH = 24,                 /* V_REFRESH  */
  YYSYMBOL_V_EDIT = 25,                    /* V_EDIT  */
  YYSYMBOL_V_DEVICES = 26,                 /* V_DEVICES  */
  YYSYMBOL_V_INSTALL_DAEMON = 27,          /* V_INSTALL_DAEMON  */
  YYSYMBOL_V_LABELCLEAR = 28,              /* V_LABELCLEAR  */
  YYSYMBOL_V_JOBS = 29,                    /* V_JOBS  */
  YYSYMBOL_V_JOB = 30,                     /* V_JOB  */
  YYSYMBOL_V_IMPORT = 31,                  /* V_IMPORT  */
  YYSYMBOL_V_FLUSH = 32,                   /* V_FLUSH  */
  YYSYMBOL_V_UPGRADE = 33,                 /* V_UPGRADE  */
  YYSYMBOL_V_REGUID = 34,                  /* V_REGUID  */
  YYSYMBOL_V_EXPORT = 35,                  /* V_EXPORT  */
  YYSYMBOL_V_STATUS = 36,                  /* V_STATUS  */
  YYSYMBOL_V_HISTORY = 37,                 /* V_HISTORY  */
  YYSYMBOL_V_SCRUB = 38,                   /* V_SCRUB  */
  YYSYMBOL_V_TRIM = 39,                    /* V_TRIM  */
  YYSYMBOL_V_INITIALIZE = 40,              /* V_INITIALIZE  */
  YYSYMBOL_V_CLEAR = 41,                   /* V_CLEAR  */
  YYSYMBOL_V_CREATE = 42,                  /* V_CREATE  */
  YYSYMBOL_V_DESTROY = 43,                 /* V_DESTROY  */
  YYSYMBOL_V_RENAME = 44,                  /* V_RENAME  */
  YYSYMBOL_V_MOUNT = 45,                   /* V_MOUNT  */
  YYSYMBOL_V_UNMOUNT = 46,                 /* V_UNMOUNT  */
  YYSYMBOL_V_PROMOTE = 47,                 /* V_PROMOTE  */
  YYSYMBOL_V_GET = 48,                     /* V_GET  */
  YYSYMBOL_V_SET = 49,                     /* V_SET  */
  YYSYMBOL_V_INHERIT = 50,                 /* V_INHERIT  */
  YYSYMBOL_V_LOAD_KEY = 51,                /* V_LOAD_KEY  */
  YYSYMBOL_V_UNLOAD_KEY = 52,              /* V_UNLOAD_KEY  */
  YYSYMBOL_V_CHANGE_KEY = 53,              /* V_CHANGE_KEY  */
  YYSYMBOL_V_SCHEDULE = 54,                /* V_SCHEDULE  */
  YYSYMBOL_V_SCHEDULES = 55,               /* V_SCHEDULES  */
  YYSYMBOL_V_LOG = 56,                     /* V_LOG  */
  YYSYMBOL_V_PEERS = 57,                   /* V_PEERS  */
  YYSYMBOL_V_REPAIR_MOUNTS = 58,           /* V_REPAIR_MOUNTS  */
  YYSYMBOL_V_ACCOUNTS = 59,                /* V_ACCOUNTS  */
  YYSYMBOL_V_AUTHORIZE_KEY = 60,           /* V_AUTHORIZE_KEY  */
  YYSYMBOL_V_EXPORT_TRUST = 61,            /* V_EXPORT_TRUST  */
  YYSYMBOL_V_ROLLBACK = 62,                /* V_ROLLBACK  */
  YYSYMBOL_V_HOLDS = 63,                   /* V_HOLDS  */
  YYSYMBOL_V_HOLD = 64,                    /* V_HOLD  */
  YYSYMBOL_V_RELEASE = 65,                 /* V_RELEASE  */
  YYSYMBOL_V_CLONE = 66,                   /* V_CLONE  */
  YYSYMBOL_V_DIFF = 67,                    /* V_DIFF  */
  YYSYMBOL_V_SEND = 68,                    /* V_SEND  */
  YYSYMBOL_V_ALLOW = 69,                   /* V_ALLOW  */
  YYSYMBOL_V_UNALLOW = 70,                 /* V_UNALLOW  */
  YYSYMBOL_V_BREAKDOWN = 71,               /* V_BREAKDOWN  */
  YYSYMBOL_V_ASSEMBLE = 72,                /* V_ASSEMBLE  */
  YYSYMBOL_V_TODIR = 73,                   /* V_TODIR  */
  YYSYMBOL_V_FROMDIR = 74,                 /* V_FROMDIR  */
  YYSYMBOL_V_RSYNC = 75,                   /* V_RSYNC  */
  YYSYMBOL_V_CP = 76,                      /* V_CP  */
  YYSYMBOL_V_DESCONOCIDO = 77,             /* V_DESCONOCIDO  */
  YYSYMBOL_YYACCEPT = 78,                  /* $accept  */
  YYSYMBOL_linea = 79,                     /* linea  */
  YYSYMBOL_orden = 80,                     /* orden  */
  YYSYMBOL_destino_opt = 81,               /* destino_opt  */
  YYSYMBOL_url_opt = 82,                   /* url_opt  */
  YYSYMBOL_fase_opt = 83,                  /* fase_opt  */
  YYSYMBOL_vdev_opt = 84,                  /* vdev_opt  */
  YYSYMBOL_nombres_prop = 85,              /* nombres_prop  */
  YYSYMBOL_textos = 86,                    /* textos  */
  YYSYMBOL_componente_texto = 87,          /* componente_texto  */
  YYSYMBOL_ranura_destino = 88,            /* ranura_destino  */
  YYSYMBOL_ruta = 89,                      /* ruta  */
  YYSYMBOL_dispositivo_lc = 90,            /* dispositivo_lc  */
  YYSYMBOL_asignaciones = 91,              /* asignaciones  */
  YYSYMBOL_palabra = 92                    /* palabra  */
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
typedef yytype_uint8 yy_state_t;

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
#define YYFINAL  157
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   283

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  78
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  15
/* YYNRULES -- Number of rules.  */
#define YYNRULES  117
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  178

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   332


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
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77
};

#if ZFSMCLIDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,    79,    79,    80,    85,    86,    87,    88,    91,    92,
      93,    94,    98,    99,   100,   101,   102,   107,   108,   109,
     110,   111,   112,   113,   114,   116,   120,   122,   123,   126,
     127,   128,   129,   130,   131,   132,   136,   137,   138,   141,
     142,   143,   144,   145,   146,   147,   151,   152,   153,   155,
     156,   157,   158,   159,   163,   164,   170,   171,   173,   175,
     176,   177,   178,   179,   180,   186,   187,   188,   189,   192,
     193,   194,   195,   200,   201,   202,   203,   204,   208,   209,
     210,   211,   215,   223,   224,   225,   233,   234,   238,   239,
     240,   241,   242,   243,   247,   248,   249,   255,   256,   262,
     263,   267,   268,   269,   280,   281,   285,   286,   290,   291,
     295,   296,   302,   303,   304,   305,   306,   307
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
  "ASIGNACION", "FASE_START", "FASE_STOP", "FASE_CANCEL", "FASE_PAUSE",
  "FASE_SUSPEND", "CARACTER_MALO", "V_CD", "V_LS", "V_PWD", "V_INFO",
  "V_HELP", "V_EXIT", "V_FORMAT", "V_YES", "V_CLS", "V_MASTER_PASSWORD",
  "V_CONNECT", "V_DISCONNECT", "V_REFRESH", "V_EDIT", "V_DEVICES",
  "V_INSTALL_DAEMON", "V_LABELCLEAR", "V_JOBS", "V_JOB", "V_IMPORT",
  "V_FLUSH", "V_UPGRADE", "V_REGUID", "V_EXPORT", "V_STATUS", "V_HISTORY",
  "V_SCRUB", "V_TRIM", "V_INITIALIZE", "V_CLEAR", "V_CREATE", "V_DESTROY",
  "V_RENAME", "V_MOUNT", "V_UNMOUNT", "V_PROMOTE", "V_GET", "V_SET",
  "V_INHERIT", "V_LOAD_KEY", "V_UNLOAD_KEY", "V_CHANGE_KEY", "V_SCHEDULE",
  "V_SCHEDULES", "V_LOG", "V_PEERS", "V_REPAIR_MOUNTS", "V_ACCOUNTS",
  "V_AUTHORIZE_KEY", "V_EXPORT_TRUST", "V_ROLLBACK", "V_HOLDS", "V_HOLD",
  "V_RELEASE", "V_CLONE", "V_DIFF", "V_SEND", "V_ALLOW", "V_UNALLOW",
  "V_BREAKDOWN", "V_ASSEMBLE", "V_TODIR", "V_FROMDIR", "V_RSYNC", "V_CP",
  "V_DESCONOCIDO", "$accept", "linea", "orden", "destino_opt", "url_opt",
  "fase_opt", "vdev_opt", "nombres_prop", "textos", "componente_texto",
  "ranura_destino", "ruta", "dispositivo_lc", "asignaciones", "palabra", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-111)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
     155,    26,    26,  -111,    26,    87,  -111,    87,    87,  -111,
    -111,    26,    26,    26,    26,    26,    26,    30,    26,     1,
       1,    26,    26,    26,    26,    26,    26,    26,    77,    77,
      32,    14,    26,    54,    26,    26,    26,     1,    26,     1,
      26,    26,    26,     1,    26,    26,    26,    26,    26,    87,
      87,    26,    26,     1,     1,    14,    62,    62,    14,    14,
      14,    14,    44,    44,    62,    14,  -111,    13,  -111,  -111,
    -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,
    -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,
    -111,  -111,  -111,  -111,    87,    87,  -111,  -111,  -111,  -111,
    -111,  -111,    77,  -111,  -111,  -111,  -111,  -111,    32,    32,
    -111,  -111,  -111,  -111,  -111,    14,  -111,  -111,  -111,    72,
    -111,  -111,  -111,  -111,    87,    10,    87,  -111,  -111,  -111,
      87,  -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,
      87,    87,    14,  -111,  -111,  -111,  -111,    14,    14,    14,
      14,  -111,  -111,  -111,  -111,  -111,    14,  -111,    87,  -111,
    -111,  -111,  -111,  -111,  -111,  -111,  -111,  -111,    21,    87,
    -111,    87,  -111,  -111,  -111,  -111,  -111,  -111
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       2,    83,    83,     6,    83,     8,    10,    13,    11,    15,
      16,    83,    83,    83,    83,    83,    83,     0,    83,    86,
      86,    83,    83,    83,    83,    83,    83,    83,    88,    88,
      94,     0,    83,     0,    83,    83,    83,    86,    83,    86,
      83,    83,    83,    86,    83,    83,    83,    83,    83,     0,
       0,    83,    83,    86,    86,     0,     0,     0,    65,    67,
      80,     0,     0,     0,     0,     0,    82,     0,     3,    84,
      85,     4,     5,     7,   112,   113,   114,   115,   116,   117,
       9,    14,    12,    17,    18,    19,    20,    21,    23,   108,
     109,    22,    24,    87,     0,    27,    29,    30,    31,    32,
      33,    34,    88,    89,    90,    91,    92,    93,    94,    94,
      96,    95,    38,   102,   103,    63,    99,   101,    42,     0,
      56,    39,    40,    41,    59,     0,     0,    43,    44,    45,
      46,    49,    50,    51,    53,    52,    54,    55,    69,    70,
       0,     0,    64,   104,    75,   105,    73,    66,    68,    79,
      81,   106,   107,    76,    77,    74,    78,     1,    25,    28,
      35,    36,    37,   100,    58,    57,    60,   110,    61,    62,
      97,    47,    71,    72,    26,   111,    98,    48
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -111,  -111,  -111,   231,   -12,   -28,   -59,  -111,   -49,  -110,
     -50,   -20,  -111,  -111,    -5
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_uint8 yydefgoto[] =
{
       0,    67,    68,    71,    94,   108,   112,   169,   115,   116,
     144,   153,    91,   168,   117
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint8 yytable[] =
{
      80,   109,    81,    82,    93,   163,   142,   146,    95,   147,
     148,   149,   150,   157,   155,   167,   156,   113,    74,   114,
      75,    76,    77,    78,    79,   124,   175,   126,   120,    69,
      70,   130,   163,    89,    90,   110,   111,   163,   163,   163,
     163,   140,   141,   154,   136,   137,   163,   151,   152,   161,
     162,   145,   145,     0,     0,     0,     0,   119,    74,   145,
      75,    76,    77,    78,    79,   143,    74,     0,    75,    76,
      77,    78,    79,     0,   160,   164,    74,     0,    75,    76,
      77,    78,    79,   103,   104,   105,   106,   107,     0,   158,
     159,    74,     0,    75,    76,    77,    78,    79,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   165,     0,     0,     0,     0,   166,
       0,   170,     0,     0,     0,   171,     0,     0,     0,     0,
       0,     0,     0,     0,     0,   172,   173,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   174,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   176,     0,   177,     1,     2,     3,
       4,     5,     6,     7,     8,     9,    10,    11,    12,    13,
      14,    15,    16,    17,    18,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    29,    30,    31,    32,    33,
      34,    35,    36,    37,    38,    39,    40,    41,    42,    43,
      44,    45,    46,    47,    48,    49,    50,    51,    52,    53,
      54,    55,    56,    57,    58,    59,    60,    61,    62,    63,
      64,    65,    66,    72,     0,    73,     0,     0,     0,     0,
       0,     0,    83,    84,    85,    86,    87,    88,     0,    92,
       0,     0,    96,    97,    98,    99,   100,   101,   102,     0,
       0,     0,     0,   118,     0,   121,   122,   123,     0,   125,
       0,   127,   128,   129,     0,   131,   132,   133,   134,   135,
       0,     0,   138,   139
};

static const yytype_int16 yycheck[] =
{
       5,    29,     7,     8,     3,   115,    55,    57,    20,    58,
      59,    60,    61,     0,    64,     5,    65,     3,     4,     5,
       6,     7,     8,     9,    10,    37,     5,    39,    33,     3,
       4,    43,   142,     3,     4,     3,     4,   147,   148,   149,
     150,    53,    54,    63,    49,    50,   156,     3,     4,   108,
     109,    56,    57,    -1,    -1,    -1,    -1,     3,     4,    64,
       6,     7,     8,     9,    10,     3,     4,    -1,     6,     7,
       8,     9,    10,    -1,   102,     3,     4,    -1,     6,     7,
       8,     9,    10,     6,     7,     8,     9,    10,    -1,    94,
      95,     4,    -1,     6,     7,     8,     9,    10,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   119,    -1,    -1,    -1,    -1,   124,
      -1,   126,    -1,    -1,    -1,   130,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   140,   141,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   158,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   169,    -1,   171,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,     2,    -1,     4,    -1,    -1,    -1,    -1,
      -1,    -1,    11,    12,    13,    14,    15,    16,    -1,    18,
      -1,    -1,    21,    22,    23,    24,    25,    26,    27,    -1,
      -1,    -1,    -1,    32,    -1,    34,    35,    36,    -1,    38,
      -1,    40,    41,    42,    -1,    44,    45,    46,    47,    48,
      -1,    -1,    51,    52
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    12,    13,    14,    15,    16,    17,    18,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    33,    34,    35,    36,    37,    38,    39,    40,
      41,    42,    43,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    53,    54,    55,    56,    57,    58,    59,    60,
      61,    62,    63,    64,    65,    66,    67,    68,    69,    70,
      71,    72,    73,    74,    75,    76,    77,    79,    80,     3,
       4,    81,    81,    81,     4,     6,     7,     8,     9,    10,
      92,    92,    92,    81,    81,    81,    81,    81,    81,     3,
       4,    90,    81,     3,    82,    82,    81,    81,    81,    81,
      81,    81,    81,     6,     7,     8,     9,    10,    83,    83,
       3,     4,    84,     3,     5,    86,    87,    92,    81,     3,
      92,    81,    81,    81,    82,    81,    82,    81,    81,    81,
      82,    81,    81,    81,    81,    81,    92,    92,    81,    81,
      82,    82,    86,     3,    88,    92,    88,    86,    86,    86,
      86,     3,     4,    89,    89,    88,    86,     0,    92,    92,
      83,    84,    84,    87,     3,    92,    92,     5,    91,    85,
      92,    92,    92,    92,    92,     5,    92,    92
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    78,    79,    79,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    80,    80,    80,    80,    80,    80,    80,
      80,    80,    80,    81,    81,    81,    82,    82,    83,    83,
      83,    83,    83,    83,    84,    84,    84,    85,    85,    86,
      86,    87,    87,    87,    88,    88,    89,    89,    90,    90,
      91,    91,    92,    92,    92,    92,    92,    92
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     0,     1,     2,     2,     1,     2,     1,     2,
       1,     1,     2,     1,     2,     1,     1,     2,     2,     2,
       2,     2,     2,     2,     2,     3,     4,     2,     3,     2,
       2,     2,     2,     2,     2,     3,     3,     3,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     3,     4,     2,
       2,     2,     2,     2,     2,     2,     2,     3,     3,     2,
       3,     3,     3,     2,     2,     1,     2,     1,     2,     2,
       2,     3,     3,     2,     2,     2,     2,     2,     2,     2,
       1,     2,     1,     0,     1,     1,     0,     1,     0,     1,
       1,     1,     1,     1,     0,     1,     1,     1,     2,     1,
       2,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     2,     1,     1,     1,     1,     1,     1
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
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1080 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_PALABRA: /* PALABRA  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1086 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_ASIGNACION: /* ASIGNACION  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1092 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_START: /* FASE_START  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1098 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_STOP: /* FASE_STOP  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1104 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_CANCEL: /* FASE_CANCEL  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1110 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_PAUSE: /* FASE_PAUSE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1116 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_FASE_SUSPEND: /* FASE_SUSPEND  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1122 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CD: /* V_CD  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1128 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_LS: /* V_LS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1134 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_PWD: /* V_PWD  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1140 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_INFO: /* V_INFO  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1146 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_HELP: /* V_HELP  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1152 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_EXIT: /* V_EXIT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1158 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_FORMAT: /* V_FORMAT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1164 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_YES: /* V_YES  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1170 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CLS: /* V_CLS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1176 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_MASTER_PASSWORD: /* V_MASTER_PASSWORD  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1182 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CONNECT: /* V_CONNECT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1188 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DISCONNECT: /* V_DISCONNECT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1194 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_REFRESH: /* V_REFRESH  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1200 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_EDIT: /* V_EDIT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1206 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DEVICES: /* V_DEVICES  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1212 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_INSTALL_DAEMON: /* V_INSTALL_DAEMON  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1218 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_LABELCLEAR: /* V_LABELCLEAR  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1224 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_JOBS: /* V_JOBS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1230 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_JOB: /* V_JOB  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1236 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_IMPORT: /* V_IMPORT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1242 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_FLUSH: /* V_FLUSH  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1248 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_UPGRADE: /* V_UPGRADE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1254 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_REGUID: /* V_REGUID  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1260 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_EXPORT: /* V_EXPORT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1266 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_STATUS: /* V_STATUS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1272 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_HISTORY: /* V_HISTORY  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1278 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SCRUB: /* V_SCRUB  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1284 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_TRIM: /* V_TRIM  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1290 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_INITIALIZE: /* V_INITIALIZE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1296 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CLEAR: /* V_CLEAR  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1302 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CREATE: /* V_CREATE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1308 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DESTROY: /* V_DESTROY  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1314 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_RENAME: /* V_RENAME  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1320 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_MOUNT: /* V_MOUNT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1326 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_UNMOUNT: /* V_UNMOUNT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1332 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_PROMOTE: /* V_PROMOTE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1338 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_GET: /* V_GET  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1344 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SET: /* V_SET  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1350 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_INHERIT: /* V_INHERIT  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1356 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_LOAD_KEY: /* V_LOAD_KEY  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1362 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_UNLOAD_KEY: /* V_UNLOAD_KEY  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1368 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CHANGE_KEY: /* V_CHANGE_KEY  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1374 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SCHEDULE: /* V_SCHEDULE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1380 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SCHEDULES: /* V_SCHEDULES  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1386 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_LOG: /* V_LOG  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1392 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_PEERS: /* V_PEERS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1398 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_REPAIR_MOUNTS: /* V_REPAIR_MOUNTS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1404 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_ACCOUNTS: /* V_ACCOUNTS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1410 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_AUTHORIZE_KEY: /* V_AUTHORIZE_KEY  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1416 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_EXPORT_TRUST: /* V_EXPORT_TRUST  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1422 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_ROLLBACK: /* V_ROLLBACK  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1428 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_HOLDS: /* V_HOLDS  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1434 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_HOLD: /* V_HOLD  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1440 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_RELEASE: /* V_RELEASE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1446 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CLONE: /* V_CLONE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1452 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DIFF: /* V_DIFF  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1458 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_SEND: /* V_SEND  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1464 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_ALLOW: /* V_ALLOW  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1470 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_UNALLOW: /* V_UNALLOW  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1476 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_BREAKDOWN: /* V_BREAKDOWN  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1482 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_ASSEMBLE: /* V_ASSEMBLE  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1488 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_TODIR: /* V_TODIR  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1494 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_FROMDIR: /* V_FROMDIR  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1500 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_RSYNC: /* V_RSYNC  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1506 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_CP: /* V_CP  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1512 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_V_DESCONOCIDO: /* V_DESCONOCIDO  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1518 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_componente_texto: /* componente_texto  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1524 "generado/gramatica.tab.c"
        break;

    case YYSYMBOL_palabra: /* palabra  */
#line 74 "gramatica.y"
            { free(((*yyvaluep).texto)); }
#line 1530 "generado/gramatica.tab.c"
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
#line 79 "gramatica.y"
                                          { res->vacia = 1; }
#line 1806 "generado/gramatica.tab.c"
    break;

  case 4: /* orden: V_CD destino_opt  */
#line 85 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1812 "generado/gramatica.tab.c"
    break;

  case 5: /* orden: V_LS destino_opt  */
#line 86 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1818 "generado/gramatica.tab.c"
    break;

  case 6: /* orden: V_PWD  */
#line 87 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1824 "generado/gramatica.tab.c"
    break;

  case 7: /* orden: V_INFO destino_opt  */
#line 88 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1830 "generado/gramatica.tab.c"
    break;

  case 8: /* orden: V_HELP  */
#line 91 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1836 "generado/gramatica.tab.c"
    break;

  case 9: /* orden: V_HELP palabra  */
#line 92 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1842 "generado/gramatica.tab.c"
    break;

  case 10: /* orden: V_EXIT  */
#line 93 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1848 "generado/gramatica.tab.c"
    break;

  case 11: /* orden: V_YES  */
#line 94 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1854 "generado/gramatica.tab.c"
    break;

  case 12: /* orden: V_YES palabra  */
#line 98 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1860 "generado/gramatica.tab.c"
    break;

  case 13: /* orden: V_FORMAT  */
#line 99 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1866 "generado/gramatica.tab.c"
    break;

  case 14: /* orden: V_FORMAT palabra  */
#line 100 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1872 "generado/gramatica.tab.c"
    break;

  case 15: /* orden: V_CLS  */
#line 101 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1878 "generado/gramatica.tab.c"
    break;

  case 16: /* orden: V_MASTER_PASSWORD  */
#line 102 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 1884 "generado/gramatica.tab.c"
    break;

  case 17: /* orden: V_CONNECT destino_opt  */
#line 107 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1890 "generado/gramatica.tab.c"
    break;

  case 18: /* orden: V_DISCONNECT destino_opt  */
#line 108 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1896 "generado/gramatica.tab.c"
    break;

  case 19: /* orden: V_REFRESH destino_opt  */
#line 109 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1902 "generado/gramatica.tab.c"
    break;

  case 20: /* orden: V_EDIT destino_opt  */
#line 110 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1908 "generado/gramatica.tab.c"
    break;

  case 21: /* orden: V_DEVICES destino_opt  */
#line 111 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1914 "generado/gramatica.tab.c"
    break;

  case 22: /* orden: V_LABELCLEAR dispositivo_lc  */
#line 112 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 1920 "generado/gramatica.tab.c"
    break;

  case 23: /* orden: V_INSTALL_DAEMON destino_opt  */
#line 113 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1926 "generado/gramatica.tab.c"
    break;

  case 24: /* orden: V_JOBS destino_opt  */
#line 114 "gramatica.y"
                                        { astVerbo(res, (yyvsp[-1].texto)); }
#line 1932 "generado/gramatica.tab.c"
    break;

  case 25: /* orden: V_JOB url_opt palabra  */
#line 116 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1938 "generado/gramatica.tab.c"
    break;

  case 26: /* orden: V_JOB url_opt palabra palabra  */
#line 120 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-3].texto)); astRanura(res, "texto", (yyvsp[-1].texto));
                                           astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1945 "generado/gramatica.tab.c"
    break;

  case 27: /* orden: V_IMPORT url_opt  */
#line 122 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 1951 "generado/gramatica.tab.c"
    break;

  case 28: /* orden: V_IMPORT url_opt palabra  */
#line 123 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 1957 "generado/gramatica.tab.c"
    break;

  case 29: /* orden: V_FLUSH destino_opt  */
#line 126 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1963 "generado/gramatica.tab.c"
    break;

  case 30: /* orden: V_UPGRADE destino_opt  */
#line 127 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1969 "generado/gramatica.tab.c"
    break;

  case 31: /* orden: V_REGUID destino_opt  */
#line 128 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1975 "generado/gramatica.tab.c"
    break;

  case 32: /* orden: V_EXPORT destino_opt  */
#line 129 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1981 "generado/gramatica.tab.c"
    break;

  case 33: /* orden: V_STATUS destino_opt  */
#line 130 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1987 "generado/gramatica.tab.c"
    break;

  case 34: /* orden: V_HISTORY destino_opt  */
#line 131 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 1993 "generado/gramatica.tab.c"
    break;

  case 35: /* orden: V_SCRUB destino_opt fase_opt  */
#line 132 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-2].texto)); }
#line 1999 "generado/gramatica.tab.c"
    break;

  case 36: /* orden: V_TRIM fase_opt vdev_opt  */
#line 136 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); }
#line 2005 "generado/gramatica.tab.c"
    break;

  case 37: /* orden: V_INITIALIZE fase_opt vdev_opt  */
#line 137 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); }
#line 2011 "generado/gramatica.tab.c"
    break;

  case 38: /* orden: V_CLEAR vdev_opt  */
#line 138 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2017 "generado/gramatica.tab.c"
    break;

  case 39: /* orden: V_MOUNT destino_opt  */
#line 141 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2023 "generado/gramatica.tab.c"
    break;

  case 40: /* orden: V_UNMOUNT destino_opt  */
#line 142 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2029 "generado/gramatica.tab.c"
    break;

  case 41: /* orden: V_PROMOTE destino_opt  */
#line 143 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2035 "generado/gramatica.tab.c"
    break;

  case 42: /* orden: V_DESTROY destino_opt  */
#line 144 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2041 "generado/gramatica.tab.c"
    break;

  case 43: /* orden: V_LOAD_KEY destino_opt  */
#line 145 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2047 "generado/gramatica.tab.c"
    break;

  case 44: /* orden: V_UNLOAD_KEY destino_opt  */
#line 146 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2053 "generado/gramatica.tab.c"
    break;

  case 45: /* orden: V_CHANGE_KEY destino_opt  */
#line 147 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2059 "generado/gramatica.tab.c"
    break;

  case 46: /* orden: V_SCHEDULE url_opt  */
#line 151 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2065 "generado/gramatica.tab.c"
    break;

  case 47: /* orden: V_SCHEDULE url_opt palabra  */
#line 152 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2071 "generado/gramatica.tab.c"
    break;

  case 48: /* orden: V_SCHEDULE url_opt palabra palabra  */
#line 153 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-3].texto)); astRanura(res, "texto", (yyvsp[-1].texto));
                                               astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2078 "generado/gramatica.tab.c"
    break;

  case 49: /* orden: V_SCHEDULES destino_opt  */
#line 155 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2084 "generado/gramatica.tab.c"
    break;

  case 50: /* orden: V_LOG destino_opt  */
#line 156 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2090 "generado/gramatica.tab.c"
    break;

  case 51: /* orden: V_PEERS destino_opt  */
#line 157 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2096 "generado/gramatica.tab.c"
    break;

  case 52: /* orden: V_ACCOUNTS destino_opt  */
#line 158 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2102 "generado/gramatica.tab.c"
    break;

  case 53: /* orden: V_REPAIR_MOUNTS destino_opt  */
#line 159 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2108 "generado/gramatica.tab.c"
    break;

  case 54: /* orden: V_AUTHORIZE_KEY palabra  */
#line 163 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2114 "generado/gramatica.tab.c"
    break;

  case 55: /* orden: V_EXPORT_TRUST palabra  */
#line 164 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2120 "generado/gramatica.tab.c"
    break;

  case 56: /* orden: V_RENAME palabra  */
#line 170 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2126 "generado/gramatica.tab.c"
    break;

  case 57: /* orden: V_RENAME URL palabra  */
#line 171 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astObjetivo(res, (yyvsp[-1].texto));
                                           astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2133 "generado/gramatica.tab.c"
    break;

  case 58: /* orden: V_RENAME URL URL  */
#line 173 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astObjetivo(res, (yyvsp[-1].texto));
                                           astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2140 "generado/gramatica.tab.c"
    break;

  case 59: /* orden: V_GET url_opt  */
#line 175 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2146 "generado/gramatica.tab.c"
    break;

  case 60: /* orden: V_GET url_opt palabra  */
#line 176 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "propiedad", (yyvsp[0].texto)); }
#line 2152 "generado/gramatica.tab.c"
    break;

  case 61: /* orden: V_SET destino_opt asignaciones  */
#line 177 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-2].texto)); }
#line 2158 "generado/gramatica.tab.c"
    break;

  case 62: /* orden: V_INHERIT url_opt nombres_prop  */
#line 178 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-2].texto)); }
#line 2164 "generado/gramatica.tab.c"
    break;

  case 63: /* orden: V_CREATE textos  */
#line 179 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2170 "generado/gramatica.tab.c"
    break;

  case 64: /* orden: V_CLONE textos  */
#line 180 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2176 "generado/gramatica.tab.c"
    break;

  case 65: /* orden: V_ALLOW  */
#line 186 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 2182 "generado/gramatica.tab.c"
    break;

  case 66: /* orden: V_ALLOW textos  */
#line 187 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2188 "generado/gramatica.tab.c"
    break;

  case 67: /* orden: V_UNALLOW  */
#line 188 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 2194 "generado/gramatica.tab.c"
    break;

  case 68: /* orden: V_UNALLOW textos  */
#line 189 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2200 "generado/gramatica.tab.c"
    break;

  case 69: /* orden: V_ROLLBACK destino_opt  */
#line 192 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2206 "generado/gramatica.tab.c"
    break;

  case 70: /* orden: V_HOLDS destino_opt  */
#line 193 "gramatica.y"
                                             { astVerbo(res, (yyvsp[-1].texto)); }
#line 2212 "generado/gramatica.tab.c"
    break;

  case 71: /* orden: V_HOLD url_opt palabra  */
#line 194 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "etiqueta", (yyvsp[0].texto)); }
#line 2218 "generado/gramatica.tab.c"
    break;

  case 72: /* orden: V_RELEASE url_opt palabra  */
#line 195 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-2].texto)); astRanura(res, "etiqueta", (yyvsp[0].texto)); }
#line 2224 "generado/gramatica.tab.c"
    break;

  case 73: /* orden: V_SEND ranura_destino  */
#line 200 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2230 "generado/gramatica.tab.c"
    break;

  case 74: /* orden: V_RSYNC ranura_destino  */
#line 201 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2236 "generado/gramatica.tab.c"
    break;

  case 75: /* orden: V_DIFF ranura_destino  */
#line 202 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2242 "generado/gramatica.tab.c"
    break;

  case 76: /* orden: V_TODIR ruta  */
#line 203 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2248 "generado/gramatica.tab.c"
    break;

  case 77: /* orden: V_FROMDIR ruta  */
#line 204 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2254 "generado/gramatica.tab.c"
    break;

  case 78: /* orden: V_CP textos  */
#line 208 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2260 "generado/gramatica.tab.c"
    break;

  case 79: /* orden: V_BREAKDOWN textos  */
#line 209 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2266 "generado/gramatica.tab.c"
    break;

  case 80: /* orden: V_BREAKDOWN  */
#line 210 "gramatica.y"
                                         { astVerbo(res, (yyvsp[0].texto)); }
#line 2272 "generado/gramatica.tab.c"
    break;

  case 81: /* orden: V_ASSEMBLE textos  */
#line 211 "gramatica.y"
                                         { astVerbo(res, (yyvsp[-1].texto)); }
#line 2278 "generado/gramatica.tab.c"
    break;

  case 82: /* orden: V_DESCONOCIDO  */
#line 215 "gramatica.y"
                                         { astVerboDesconocido(res, (yyvsp[0].texto)); }
#line 2284 "generado/gramatica.tab.c"
    break;

  case 84: /* destino_opt: URL  */
#line 224 "gramatica.y"
                                         { astObjetivo(res, (yyvsp[0].texto)); }
#line 2290 "generado/gramatica.tab.c"
    break;

  case 85: /* destino_opt: PALABRA  */
#line 225 "gramatica.y"
                                         { astObjetivo(res, (yyvsp[0].texto)); }
#line 2296 "generado/gramatica.tab.c"
    break;

  case 87: /* url_opt: URL  */
#line 234 "gramatica.y"
                                         { astObjetivo(res, (yyvsp[0].texto)); }
#line 2302 "generado/gramatica.tab.c"
    break;

  case 89: /* fase_opt: FASE_START  */
#line 239 "gramatica.y"
                                         { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 2308 "generado/gramatica.tab.c"
    break;

  case 90: /* fase_opt: FASE_STOP  */
#line 240 "gramatica.y"
                                         { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 2314 "generado/gramatica.tab.c"
    break;

  case 91: /* fase_opt: FASE_CANCEL  */
#line 241 "gramatica.y"
                                         { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 2320 "generado/gramatica.tab.c"
    break;

  case 92: /* fase_opt: FASE_PAUSE  */
#line 242 "gramatica.y"
                                         { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 2326 "generado/gramatica.tab.c"
    break;

  case 93: /* fase_opt: FASE_SUSPEND  */
#line 243 "gramatica.y"
                                         { astRanura(res, "fase", (yyvsp[0].texto)); }
#line 2332 "generado/gramatica.tab.c"
    break;

  case 95: /* vdev_opt: PALABRA  */
#line 248 "gramatica.y"
                                         { astRanura(res, "disco", (yyvsp[0].texto)); }
#line 2338 "generado/gramatica.tab.c"
    break;

  case 96: /* vdev_opt: URL  */
#line 249 "gramatica.y"
                                         { astRanura(res, "disco", (yyvsp[0].texto)); }
#line 2344 "generado/gramatica.tab.c"
    break;

  case 97: /* nombres_prop: palabra  */
#line 255 "gramatica.y"
                                         { astRanura(res, "props", (yyvsp[0].texto)); }
#line 2350 "generado/gramatica.tab.c"
    break;

  case 98: /* nombres_prop: nombres_prop palabra  */
#line 256 "gramatica.y"
                                         { astRanura(res, "props", (yyvsp[0].texto)); }
#line 2356 "generado/gramatica.tab.c"
    break;

  case 99: /* textos: componente_texto  */
#line 262 "gramatica.y"
                                         { astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2362 "generado/gramatica.tab.c"
    break;

  case 100: /* textos: textos componente_texto  */
#line 263 "gramatica.y"
                                         { astRanura(res, "texto", (yyvsp[0].texto)); }
#line 2368 "generado/gramatica.tab.c"
    break;

  case 101: /* componente_texto: palabra  */
#line 267 "gramatica.y"
                   { (yyval.texto) = (yyvsp[0].texto); }
#line 2374 "generado/gramatica.tab.c"
    break;

  case 102: /* componente_texto: URL  */
#line 268 "gramatica.y"
                   { (yyval.texto) = (yyvsp[0].texto); }
#line 2380 "generado/gramatica.tab.c"
    break;

  case 103: /* componente_texto: ASIGNACION  */
#line 269 "gramatica.y"
                   { (yyval.texto) = (yyvsp[0].texto); }
#line 2386 "generado/gramatica.tab.c"
    break;

  case 104: /* ranura_destino: URL  */
#line 280 "gramatica.y"
                                         { astRanura(res, "destino", (yyvsp[0].texto)); }
#line 2392 "generado/gramatica.tab.c"
    break;

  case 105: /* ranura_destino: palabra  */
#line 281 "gramatica.y"
                                         { astRanura(res, "destino", (yyvsp[0].texto)); }
#line 2398 "generado/gramatica.tab.c"
    break;

  case 106: /* ruta: URL  */
#line 285 "gramatica.y"
                                         { astRanura(res, "ruta", (yyvsp[0].texto)); }
#line 2404 "generado/gramatica.tab.c"
    break;

  case 107: /* ruta: PALABRA  */
#line 286 "gramatica.y"
                                         { astRanura(res, "ruta", (yyvsp[0].texto)); }
#line 2410 "generado/gramatica.tab.c"
    break;

  case 108: /* dispositivo_lc: URL  */
#line 290 "gramatica.y"
                                         { astRanura(res, "dispositivo", (yyvsp[0].texto)); }
#line 2416 "generado/gramatica.tab.c"
    break;

  case 109: /* dispositivo_lc: PALABRA  */
#line 291 "gramatica.y"
                                         { astRanura(res, "dispositivo", (yyvsp[0].texto)); }
#line 2422 "generado/gramatica.tab.c"
    break;

  case 110: /* asignaciones: ASIGNACION  */
#line 295 "gramatica.y"
                                         { astRanura(res, "props", (yyvsp[0].texto)); }
#line 2428 "generado/gramatica.tab.c"
    break;

  case 111: /* asignaciones: asignaciones ASIGNACION  */
#line 296 "gramatica.y"
                                         { astRanura(res, "props", (yyvsp[0].texto)); }
#line 2434 "generado/gramatica.tab.c"
    break;

  case 112: /* palabra: PALABRA  */
#line 302 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 2440 "generado/gramatica.tab.c"
    break;

  case 113: /* palabra: FASE_START  */
#line 303 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 2446 "generado/gramatica.tab.c"
    break;

  case 114: /* palabra: FASE_STOP  */
#line 304 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 2452 "generado/gramatica.tab.c"
    break;

  case 115: /* palabra: FASE_CANCEL  */
#line 305 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 2458 "generado/gramatica.tab.c"
    break;

  case 116: /* palabra: FASE_PAUSE  */
#line 306 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 2464 "generado/gramatica.tab.c"
    break;

  case 117: /* palabra: FASE_SUSPEND  */
#line 307 "gramatica.y"
                     { (yyval.texto) = (yyvsp[0].texto); }
#line 2470 "generado/gramatica.tab.c"
    break;


#line 2474 "generado/gramatica.tab.c"

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

#line 311 "gramatica.y"


void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg) {
    (void)scanner;
    astError(res, msg);
}

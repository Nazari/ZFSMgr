/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_ZFSMCLI_GENERADO_GRAMATICA_TAB_H_INCLUDED
# define YY_ZFSMCLI_GENERADO_GRAMATICA_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef ZFSMCLIDEBUG
# if defined YYDEBUG
#if YYDEBUG
#   define ZFSMCLIDEBUG 1
#  else
#   define ZFSMCLIDEBUG 0
#  endif
# else /* ! defined YYDEBUG */
#  define ZFSMCLIDEBUG 0
# endif /* ! defined YYDEBUG */
#endif  /* ! defined ZFSMCLIDEBUG */
#if ZFSMCLIDEBUG
extern int zfsmclidebug;
#endif
/* "%code requires" blocks.  */
#line 30 "gramatica.y"

#include "gramatica_ast.h"

#line 61 "generado/gramatica.tab.h"

/* Token kinds.  */
#ifndef ZFSMCLITOKENTYPE
# define ZFSMCLITOKENTYPE
  enum zfsmclitokentype
  {
    ZFSMCLIEMPTY = -2,
    ZFSMCLIEOF = 0,                /* "end of file"  */
    ZFSMCLIerror = 256,            /* error  */
    ZFSMCLIUNDEF = 257,            /* "invalid token"  */
    URL = 258,                     /* URL  */
    PALABRA = 259,                 /* PALABRA  */
    ASIGNACION = 260,              /* ASIGNACION  */
    OPCION_LARGA = 261,            /* OPCION_LARGA  */
    OPCION_CORTA = 262,            /* OPCION_CORTA  */
    FASE_START = 263,              /* FASE_START  */
    FASE_STOP = 264,               /* FASE_STOP  */
    FASE_CANCEL = 265,             /* FASE_CANCEL  */
    FASE_PAUSE = 266,              /* FASE_PAUSE  */
    FASE_SUSPEND = 267,            /* FASE_SUSPEND  */
    CARACTER_MALO = 268,           /* CARACTER_MALO  */
    V_NADA = 269,                  /* V_NADA  */
    V_TEXTO = 270,                 /* V_TEXTO  */
    V_CUALQUIERA = 271,            /* V_CUALQUIERA  */
    V_CONN = 272,                  /* V_CONN  */
    V_CONN_TEXTO = 273,            /* V_CONN_TEXTO  */
    V_POOL = 274,                  /* V_POOL  */
    V_POOL_FASE = 275,             /* V_POOL_FASE  */
    V_POOL_FASE_VDEV = 276,        /* V_POOL_FASE_VDEV  */
    V_POOL_VDEV = 277,             /* V_POOL_VDEV  */
    V_DS = 278,                    /* V_DS  */
    V_DS_TEXTO_OPC = 279,          /* V_DS_TEXTO_OPC  */
    V_DS_ASIGNA = 280,             /* V_DS_ASIGNA  */
    V_DS_URL = 281,                /* V_DS_URL  */
    V_DS_RUTA = 282,               /* V_DS_RUTA  */
    V_DS_TEXTO_MAS = 283,          /* V_DS_TEXTO_MAS  */
    V_SNAP = 284,                  /* V_SNAP  */
    V_SNAP_TEXTO = 285,            /* V_SNAP_TEXTO  */
    V_SNAP_URL = 286               /* V_SNAP_URL  */
  };
  typedef enum zfsmclitokentype zfsmclitoken_kind_t;
#endif

/* Value type.  */
#if ! defined ZFSMCLISTYPE && ! defined ZFSMCLISTYPE_IS_DECLARED
union ZFSMCLISTYPE
{
#line 46 "gramatica.y"

    char* texto;

#line 113 "generado/gramatica.tab.h"

};
typedef union ZFSMCLISTYPE ZFSMCLISTYPE;
# define ZFSMCLISTYPE_IS_TRIVIAL 1
# define ZFSMCLISTYPE_IS_DECLARED 1
#endif




int zfsmcliparse (void* scanner, AnalisisCli* res);

/* "%code provides" blocks.  */
#line 35 "gramatica.y"

int zfsmclilex(ZFSMCLISTYPE* yylval, void* scanner);
void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg);

#line 132 "generado/gramatica.tab.h"

#endif /* !YY_ZFSMCLI_GENERADO_GRAMATICA_TAB_H_INCLUDED  */

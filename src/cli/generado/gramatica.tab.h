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
#line 31 "gramatica.y"

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
    FASE_START = 261,              /* FASE_START  */
    FASE_STOP = 262,               /* FASE_STOP  */
    FASE_CANCEL = 263,             /* FASE_CANCEL  */
    FASE_PAUSE = 264,              /* FASE_PAUSE  */
    FASE_SUSPEND = 265,            /* FASE_SUSPEND  */
    CARACTER_MALO = 266,           /* CARACTER_MALO  */
    V_CD = 267,                    /* V_CD  */
    V_LS = 268,                    /* V_LS  */
    V_PWD = 269,                   /* V_PWD  */
    V_INFO = 270,                  /* V_INFO  */
    V_HELP = 271,                  /* V_HELP  */
    V_EXIT = 272,                  /* V_EXIT  */
    V_FORMAT = 273,                /* V_FORMAT  */
    V_YES = 274,                   /* V_YES  */
    V_CLS = 275,                   /* V_CLS  */
    V_CONNECT = 276,               /* V_CONNECT  */
    V_DISCONNECT = 277,            /* V_DISCONNECT  */
    V_REFRESH = 278,               /* V_REFRESH  */
    V_EDIT = 279,                  /* V_EDIT  */
    V_DEVICES = 280,               /* V_DEVICES  */
    V_INSTALL_DAEMON = 281,        /* V_INSTALL_DAEMON  */
    V_JOBS = 282,                  /* V_JOBS  */
    V_JOB = 283,                   /* V_JOB  */
    V_IMPORT = 284,                /* V_IMPORT  */
    V_FLUSH = 285,                 /* V_FLUSH  */
    V_UPGRADE = 286,               /* V_UPGRADE  */
    V_REGUID = 287,                /* V_REGUID  */
    V_EXPORT = 288,                /* V_EXPORT  */
    V_STATUS = 289,                /* V_STATUS  */
    V_HISTORY = 290,               /* V_HISTORY  */
    V_SCRUB = 291,                 /* V_SCRUB  */
    V_TRIM = 292,                  /* V_TRIM  */
    V_INITIALIZE = 293,            /* V_INITIALIZE  */
    V_CLEAR = 294,                 /* V_CLEAR  */
    V_CREATE = 295,                /* V_CREATE  */
    V_DESTROY = 296,               /* V_DESTROY  */
    V_RENAME = 297,                /* V_RENAME  */
    V_MOUNT = 298,                 /* V_MOUNT  */
    V_UNMOUNT = 299,               /* V_UNMOUNT  */
    V_PROMOTE = 300,               /* V_PROMOTE  */
    V_GET = 301,                   /* V_GET  */
    V_SET = 302,                   /* V_SET  */
    V_LOAD_KEY = 303,              /* V_LOAD_KEY  */
    V_UNLOAD_KEY = 304,            /* V_UNLOAD_KEY  */
    V_CHANGE_KEY = 305,            /* V_CHANGE_KEY  */
    V_SCHEDULE = 306,              /* V_SCHEDULE  */
    V_SCHEDULES = 307,             /* V_SCHEDULES  */
    V_ROLLBACK = 308,              /* V_ROLLBACK  */
    V_HOLDS = 309,                 /* V_HOLDS  */
    V_HOLD = 310,                  /* V_HOLD  */
    V_RELEASE = 311,               /* V_RELEASE  */
    V_CLONE = 312,                 /* V_CLONE  */
    V_DIFF = 313,                  /* V_DIFF  */
    V_COPY = 314,                  /* V_COPY  */
    V_ALLOW = 315,                 /* V_ALLOW  */
    V_UNALLOW = 316,               /* V_UNALLOW  */
    V_BREAKDOWN = 317,             /* V_BREAKDOWN  */
    V_ASSEMBLE = 318,              /* V_ASSEMBLE  */
    V_TODIR = 319,                 /* V_TODIR  */
    V_FROMDIR = 320,               /* V_FROMDIR  */
    V_RSYNC = 321,                 /* V_RSYNC  */
    V_DESCONOCIDO = 322            /* V_DESCONOCIDO  */
  };
  typedef enum zfsmclitokentype zfsmclitoken_kind_t;
#endif

/* Value type.  */
#if ! defined ZFSMCLISTYPE && ! defined ZFSMCLISTYPE_IS_DECLARED
union ZFSMCLISTYPE
{
#line 41 "gramatica.y"

    char* texto;

#line 149 "generado/gramatica.tab.h"

};
typedef union ZFSMCLISTYPE ZFSMCLISTYPE;
# define ZFSMCLISTYPE_IS_TRIVIAL 1
# define ZFSMCLISTYPE_IS_DECLARED 1
#endif




int zfsmcliparse (void* scanner, AnalisisCli* res);

/* "%code provides" blocks.  */
#line 46 "gramatica.y"

int zfsmclilex(ZFSMCLISTYPE* yylval, void* scanner);
void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg);

#line 168 "generado/gramatica.tab.h"

#endif /* !YY_ZFSMCLI_GENERADO_GRAMATICA_TAB_H_INCLUDED  */

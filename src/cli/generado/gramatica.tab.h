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
    V_MASTER_PASSWORD = 276,       /* V_MASTER_PASSWORD  */
    V_MASTER_KEY = 277,            /* V_MASTER_KEY  */
    V_CONNECT = 278,               /* V_CONNECT  */
    V_DISCONNECT = 279,            /* V_DISCONNECT  */
    V_REFRESH = 280,               /* V_REFRESH  */
    V_EDIT = 281,                  /* V_EDIT  */
    V_DEVICES = 282,               /* V_DEVICES  */
    V_INSTALL_DAEMON = 283,        /* V_INSTALL_DAEMON  */
    V_LABELCLEAR = 284,            /* V_LABELCLEAR  */
    V_JOBS = 285,                  /* V_JOBS  */
    V_JOB = 286,                   /* V_JOB  */
    V_IMPORT = 287,                /* V_IMPORT  */
    V_FLUSH = 288,                 /* V_FLUSH  */
    V_UPGRADE = 289,               /* V_UPGRADE  */
    V_REGUID = 290,                /* V_REGUID  */
    V_EXPORT = 291,                /* V_EXPORT  */
    V_STATUS = 292,                /* V_STATUS  */
    V_HISTORY = 293,               /* V_HISTORY  */
    V_SCRUB = 294,                 /* V_SCRUB  */
    V_TRIM = 295,                  /* V_TRIM  */
    V_INITIALIZE = 296,            /* V_INITIALIZE  */
    V_CLEAR = 297,                 /* V_CLEAR  */
    V_CREATE = 298,                /* V_CREATE  */
    V_DESTROY = 299,               /* V_DESTROY  */
    V_RENAME = 300,                /* V_RENAME  */
    V_MOUNT = 301,                 /* V_MOUNT  */
    V_UNMOUNT = 302,               /* V_UNMOUNT  */
    V_PROMOTE = 303,               /* V_PROMOTE  */
    V_GET = 304,                   /* V_GET  */
    V_SET = 305,                   /* V_SET  */
    V_INHERIT = 306,               /* V_INHERIT  */
    V_LOAD_KEY = 307,              /* V_LOAD_KEY  */
    V_UNLOAD_KEY = 308,            /* V_UNLOAD_KEY  */
    V_CHANGE_KEY = 309,            /* V_CHANGE_KEY  */
    V_SCHEDULE = 310,              /* V_SCHEDULE  */
    V_SCHEDULES = 311,             /* V_SCHEDULES  */
    V_SESSIONS = 312,              /* V_SESSIONS  */
    V_OVERVIEW = 313,              /* V_OVERVIEW  */
    V_PROBE = 314,                 /* V_PROBE  */
    V_LOG = 315,                   /* V_LOG  */
    V_PEERS = 316,                 /* V_PEERS  */
    V_REPAIR_MOUNTS = 317,         /* V_REPAIR_MOUNTS  */
    V_ACCOUNTS = 318,              /* V_ACCOUNTS  */
    V_AUTHORIZE_KEY = 319,         /* V_AUTHORIZE_KEY  */
    V_EXPORT_TRUST = 320,          /* V_EXPORT_TRUST  */
    V_ROLLBACK = 321,              /* V_ROLLBACK  */
    V_HOLDS = 322,                 /* V_HOLDS  */
    V_HOLD = 323,                  /* V_HOLD  */
    V_RELEASE = 324,               /* V_RELEASE  */
    V_CLONE = 325,                 /* V_CLONE  */
    V_DIFF = 326,                  /* V_DIFF  */
    V_SEND = 327,                  /* V_SEND  */
    V_ALLOW = 328,                 /* V_ALLOW  */
    V_UNALLOW = 329,               /* V_UNALLOW  */
    V_BREAKDOWN = 330,             /* V_BREAKDOWN  */
    V_ASSEMBLE = 331,              /* V_ASSEMBLE  */
    V_TODIR = 332,                 /* V_TODIR  */
    V_FROMDIR = 333,               /* V_FROMDIR  */
    V_RSYNC = 334,                 /* V_RSYNC  */
    V_CP = 335,                    /* V_CP  */
    V_DESCONOCIDO = 336            /* V_DESCONOCIDO  */
  };
  typedef enum zfsmclitokentype zfsmclitoken_kind_t;
#endif

/* Value type.  */
#if ! defined ZFSMCLISTYPE && ! defined ZFSMCLISTYPE_IS_DECLARED
union ZFSMCLISTYPE
{
#line 41 "gramatica.y"

    char* texto;

#line 163 "generado/gramatica.tab.h"

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

#line 182 "generado/gramatica.tab.h"

#endif /* !YY_ZFSMCLI_GENERADO_GRAMATICA_TAB_H_INCLUDED  */

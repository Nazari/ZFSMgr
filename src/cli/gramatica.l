%{
/* Analizador léxico de la línea del intérprete.
 *
 * Lo que decide todo lo demás está aquí: **una URL y una palabra son componentes
 * DISTINTOS**, y se distinguen por su forma, no preguntándole nada a ninguna máquina.
 *
 * Con eso, `scrub stop` y `scrub /local/tank` dejan de ser la misma frase y la gramática
 * puede resolverlos sin reglas de precedencia escritas a mano. Antes hacía falta consultar
 * al daemon qué pools existían para saber si «tank» era un destino o un argumento; ese
 * viaje desaparece.
 *
 * Ver docs/gramatica_cli.md.
 */
#include "gramatica_ast.h"
#include "gramatica.tab.h"

/* flex nombra el tipo del valor `YYSTYPE`; bison, con `api.prefix`, lo llama
 * `ZFSMCLISTYPE`. Sin este puente el generado no compila. */
#define YYSTYPE ZFSMCLISTYPE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* La tabla de VERBOS. El primer componente de la línea se busca aquí y se devuelve su
 * token; a partir de ahí, las palabras son palabras.
 *
 * Está en el léxico y no en otro sitio a propósito: junto a la gramática, un verbo nuevo se
 * ve de un vistazo —una fila aquí, un token y una producción allá— y todo lo que define la
 * sintaxis queda en estos dos ficheros. `zfsmgr_gramatica_cli_test` comprueba que ninguna
 * orden del catálogo se queda sin fila.
 */
static const struct { const char* nombre; int token; } kVerbos[] = {
    {"cd", V_CD}, {"ls", V_LS}, {"pwd", V_PWD}, {"info", V_INFO},
    {"help", V_HELP}, {"exit", V_EXIT}, {"quit", V_EXIT}, {"format", V_FORMAT}, {"yes", V_YES},
    {"cls", V_CLS},
    {"connect", V_CONNECT}, {"disconnect", V_DISCONNECT}, {"refresh", V_REFRESH},
    {"edit", V_EDIT}, {"devices", V_DEVICES}, {"install-daemon", V_INSTALL_DAEMON},
    {"jobs", V_JOBS}, {"job", V_JOB}, {"import", V_IMPORT},
    {"flush", V_FLUSH}, {"upgrade", V_UPGRADE}, {"reguid", V_REGUID}, {"export", V_EXPORT},
    {"status", V_STATUS}, {"history", V_HISTORY},
    {"scrub", V_SCRUB}, {"trim", V_TRIM}, {"initialize", V_INITIALIZE}, {"clear", V_CLEAR},
    {"create", V_CREATE}, {"destroy", V_DESTROY}, {"rename", V_RENAME},
    {"mount", V_MOUNT}, {"unmount", V_UNMOUNT}, {"promote", V_PROMOTE},
    {"get", V_GET}, {"set", V_SET}, {"load-key", V_LOAD_KEY}, {"unload-key", V_UNLOAD_KEY},
    {"rollback", V_ROLLBACK}, {"holds", V_HOLDS}, {"hold", V_HOLD}, {"release", V_RELEASE},
    {"clone", V_CLONE}, {"diff", V_DIFF}, {"copy", V_COPY},
    {"allow", V_ALLOW}, {"unallow", V_UNALLOW},
    {"breakdown", V_BREAKDOWN}, {"assemble", V_ASSEMBLE},
    {"todir", V_TODIR}, {"fromdir", V_FROMDIR}, {"rsync", V_RSYNC},
    {0, 0}
};

static int tokenDeVerbo(const char* v) {
    int i;
    for (i = 0; kVerbos[i].nombre; ++i) {
        if (strcmp(kVerbos[i].nombre, v) == 0) {
            return kVerbos[i].token;
        }
    }
    return V_DESCONOCIDO;
}

typedef struct {
    int arrancado;
    AnalisisCli* res;
    ZfsmCliLlevaValor llevaValor;
    void* ctx;
    char verbo[64];
    char opcionPendiente[64];
} ContextoLex;

static char* copia(const char* s, int n) {
    char* p = (char*)malloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

/* Quita las comillas y deshace los escapes. Un punto de montaje con espacios es UN
 * componente, no dos. */
static char* sinComillas(const char* s, int n) {
    char* p = (char*)malloc((size_t)n + 1);
    int i = 1, j = 0;              /* se salta la comilla de apertura */
    for (; i < n - 1; ++i) {
        if (s[i] == '\\' && i + 1 < n - 1) {
            ++i;
        }
        p[j++] = s[i];
    }
    p[j] = 0;
    return p;
}
%}

/* `nounistd`: sin él, el analizador generado incluye <unistd.h>, y en MinGW ese arrastra
 * un <process.h> del sistema que choca con el `process.h` de la capa base —el cruce a
 * Windows fallaba con «cstdint: No such file or directory» dentro de unistd.h—. */
%option reentrant bison-bridge noyywrap nounput noinput never-interactive nounistd
%option extra-type="void*"
%option prefix="zfsmcli"

/* El primer componente de la línea es el VERBO y se trata aparte; y tras una opción que
 * lleva valor, el siguiente componente ES ese valor. */
%x VERBO VALOR VALOR2

ESPACIO     [ \t]+
/* Lo que hace URL a un componente: el esquema, una barra en cualquier sitio, empezar por
 * `@` o `#`, o ser uno de los tres atajos de navegación. */
URLTOK      ("zfsm://"[^ \t]*)|([^ \t=]*"/"[^ \t]*)|("@"[^ \t=]+)|("#"[^ \t=]*)|"."|".."|"-"
ASIGNA      [^ \t=/-][^ \t=]*"="[^ \t]*
LARGA       "--"[A-Za-z][A-Za-z0-9-]*
CORTA       "-"[A-Za-z]+
PALABRA     [^ \t=][^ \t]*

%%

%{
    /* Se empieza esperando el VERBO, y una sola vez.
     *
     * Con una marca explícita y no mirando `yy_start`: `INITIAL` vale 1, así que la
     * comprobación obvia —«si estoy en el estado inicial, empieza por el verbo»— volvía a
     * entrar en VERBO en CADA token y no se analizaba nada. */
    {
        ContextoLex* cx0 = (ContextoLex*)zfsmcliget_extra(yyscanner);
        if (cx0 && !cx0->arrancado) {
            cx0->arrancado = 1;
            BEGIN(VERBO);
        }
    }
%}

<VERBO>{ESPACIO}    { /* separador delante del verbo */ }
<VERBO>[^ \t]+      {
                        ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                        BEGIN(INITIAL);
                        yylval->texto = copia(yytext, yyleng);
                        if (cx) {
                            snprintf(cx->verbo, sizeof(cx->verbo), "%s", yylval->texto);
                        }
                        return tokenDeVerbo(yylval->texto);
                    }

  /* El valor de la opción anterior. No llega a la gramática: se anota y se sigue.
   *
   * ENTRECOMILLADO, es UN valor aunque lleve espacios: `--flags "-w -L"`, o un punto de
   * montaje con espacios. Sin esta regla se cortaba en el primer espacio y el resto se
   * leía como un componente suelto de la orden —el valor quedaba en «"-w» y la orden
   * fallaba por sintaxis en «-L"»—, que es lo mismo que ya pasaba con los componentes
   * corrientes antes de que se les quitaran las comillas. */
<VALOR>{ESPACIO}    { /* separador */ }
<VALOR>\"([^"\\]|\\.)*\"|'([^'\\]|\\.)*' {
                        ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                        BEGIN(INITIAL);
                        astOpcion(cx->res, copia(cx->opcionPendiente, (int)strlen(cx->opcionPendiente)),
                                  sinComillas(yytext, yyleng));
                    }
<VALOR>[^ \t]+      {
                        ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                        BEGIN(INITIAL);
                        astOpcion(cx->res, copia(cx->opcionPendiente, (int)strlen(cx->opcionPendiente)),
                                  copia(yytext, yyleng));
                    }
<VALOR2>{ESPACIO}   { /* separador */ }
<VALOR2>\"([^"\\]|\\.)*\"|'([^'\\]|\\.)*' {
                        ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                        BEGIN(INITIAL);
                        astOpcionRepetida(cx->res,
                                          copia(cx->opcionPendiente, (int)strlen(cx->opcionPendiente)),
                                          sinComillas(yytext, yyleng));
                    }
<VALOR2>[^ \t]+     {
                        ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                        BEGIN(INITIAL);
                        astOpcionRepetida(cx->res,
                                          copia(cx->opcionPendiente, (int)strlen(cx->opcionPendiente)),
                                          copia(yytext, yyleng));
                    }

{ESPACIO}       { /* separador */ }

"start"         { yylval->texto = copia(yytext, yyleng); return FASE_START; }
"stop"          { yylval->texto = copia(yytext, yyleng); return FASE_STOP; }
"cancel"        { yylval->texto = copia(yytext, yyleng); return FASE_CANCEL; }
"pause"         { yylval->texto = copia(yytext, yyleng); return FASE_PAUSE; }
"suspend"       { yylval->texto = copia(yytext, yyleng); return FASE_SUSPEND; }

  /* Las OPCIONES no pasan por la gramática: se anotan aquí. Su posición no significa nada
   * —`allow --user u perms` y `allow perms --user u` son la misma orden—, así que meterlas
   * en las producciones obligaba a escribirlas al final. Lo hacían: `allow --user linarese
   * snapshot` no se analizaba. */
{LARGA}         {
                    ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                    char* nombre = copia(yytext + 2, yyleng - 2);
                    if (cx->llevaValor && cx->llevaValor(cx->verbo, nombre, cx->ctx)) {
                        snprintf(cx->opcionPendiente, sizeof(cx->opcionPendiente), "%s", nombre);
                        free(nombre);
                        BEGIN(VALOR);
                    } else {
                        astOpcion(cx->res, nombre, 0);
                    }
                }
{CORTA}/{ESPACIO}{ASIGNA} {
                    /* `-o ashift=12`: opción corta CON valor, repetible. */
                    ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                    snprintf(cx->opcionPendiente, sizeof(cx->opcionPendiente), "%s", yytext);
                    BEGIN(VALOR2);
                }
{CORTA}         {
                    /* Una bandera corta también puede llevar valor —`import -d /dev`,
                     * `trim -r 100M`—: lo dicen las banderas nativas declaradas para ese
                     * verbo. Sin esto, el valor se leía como un argumento suelto. */
                    ContextoLex* cx = (ContextoLex*)zfsmcliget_extra(yyscanner);
                    char* nombre = copia(yytext, yyleng);
                    if (cx->llevaValor && cx->llevaValor(cx->verbo, nombre, cx->ctx)) {
                        /* Sin los guiones, igual que las largas: las opciones se guardan
                         * con UNA sola forma de clave. Guardándola con guiones, buscarla
                         * fallaba y el mensaje de error salía con tres. */
                        snprintf(cx->opcionPendiente, sizeof(cx->opcionPendiente), "%s",
                                 nombre + strspn(nombre, "-"));
                        free(nombre);
                        BEGIN(VALOR);
                    } else {
                        astBandera(cx->res, nombre);
                    }
                }
{URLTOK}        { yylval->texto = copia(yytext, yyleng); return URL; }
{ASIGNA}        { yylval->texto = copia(yytext, yyleng); return ASIGNACION; }

\"([^"\\]|\\.)*\"   { yylval->texto = sinComillas(yytext, yyleng); return PALABRA; }
'([^'\\]|\\.)*'     { yylval->texto = sinComillas(yytext, yyleng); return PALABRA; }

{PALABRA}       { yylval->texto = copia(yytext, yyleng); return PALABRA; }

.               { return CARACTER_MALO; }

%%

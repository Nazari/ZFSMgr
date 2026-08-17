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

#include <stdlib.h>
#include <string.h>

/* La clase del verbo. El léxico la pregunta al catálogo para el PRIMER componente de la
 * línea; a partir de ahí, las palabras son palabras.
 *
 * Es realimentación léxica, sí, y es lo que permite tener UNA producción por FORMA de orden
 * en vez de una por verbo: añadir una orden es una fila en una tabla y no una regla nueva.
 */
typedef struct {
    ZfsmCliClaseDeVerbo clase;
    void* ctx;
    int arrancado;
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

%option reentrant bison-bridge noyywrap nounput noinput never-interactive
%option extra-type="void*"
%option prefix="zfsmcli"

/* El primer componente de la línea es el VERBO y se trata aparte. */
%x VERBO

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
                        return cx->clase(yylval->texto, cx->ctx);
                    }

{ESPACIO}       { /* separador */ }

"start"         { yylval->texto = copia(yytext, yyleng); return FASE_START; }
"stop"          { yylval->texto = copia(yytext, yyleng); return FASE_STOP; }
"cancel"        { yylval->texto = copia(yytext, yyleng); return FASE_CANCEL; }
"pause"         { yylval->texto = copia(yytext, yyleng); return FASE_PAUSE; }
"suspend"       { yylval->texto = copia(yytext, yyleng); return FASE_SUSPEND; }

{LARGA}         { yylval->texto = copia(yytext + 2, yyleng - 2); return OPCION_LARGA; }
{CORTA}         { yylval->texto = copia(yytext, yyleng); return OPCION_CORTA; }
{URLTOK}        { yylval->texto = copia(yytext, yyleng); return URL; }
{ASIGNA}        { yylval->texto = copia(yytext, yyleng); return ASIGNACION; }

\"([^"\\]|\\.)*\"   { yylval->texto = sinComillas(yytext, yyleng); return PALABRA; }
'([^'\\]|\\.)*'     { yylval->texto = sinComillas(yytext, yyleng); return PALABRA; }

{PALABRA}       { yylval->texto = copia(yytext, yyleng); return PALABRA; }

.               { return CARACTER_MALO; }

%%

%{
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
%}

/* El tipo va también en la CABECERA generada: la firma de `zfsmcliparse` lo menciona. */
%code requires {
#include "gramatica_ast.h"
}

/* Y estas van DESPUÉS del %union, porque usan su tipo. */
%code provides {
int zfsmclilex(ZFSMCLISTYPE* yylval, void* scanner);
void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg);
}

%define api.pure full
%define api.prefix {zfsmcli}
%lex-param   {void* scanner}
%parse-param {void* scanner} {AnalisisCli* res}
%expect 0

%union {
    char* texto;
}

%token <texto> URL PALABRA ASIGNACION OPCION_LARGA OPCION_CORTA
%token <texto> FASE_START FASE_STOP FASE_CANCEL FASE_PAUSE FASE_SUSPEND
%token CARACTER_MALO

/* Una clase de verbo por FORMA. El léxico decide cuál devuelve mirando el catálogo. */
%token <texto> V_NADA          /* sin objetivo ni ranuras: help, exit               */
%token <texto> V_TEXTO         /* sin objetivo, una palabra: format, help <orden>   */
%token <texto> V_CUALQUIERA    /* vale en cualquier nodo: ls, cd, info              */
%token <texto> V_CONN          /* @conexion: install-daemon, refresh, jobs          */
%token <texto> V_CONN_TEXTO    /* @conexion <palabra>: import, job                  */
%token <texto> V_POOL          /* @pool: flush, upgrade, status                     */
%token <texto> V_POOL_FASE     /* @pool [fase]: scrub                               */
%token <texto> V_POOL_FASE_VDEV/* @pool [fase] [vdev]: trim, initialize             */
%token <texto> V_POOL_VDEV     /* @pool [vdev]: clear                               */
%token <texto> V_DS            /* @dataset: mount, promote, load-key                */
%token <texto> V_DS_TEXTO_OPC  /* @dataset [palabra]: get                           */
%token <texto> V_DS_ASIGNA     /* @dataset asignacion+: set                         */
%token <texto> V_DS_URL        /* @dataset <url>: rsync, clone, diff                */
%token <texto> V_DS_RUTA       /* @dataset <ruta>: todir, fromdir                   */
%token <texto> V_DS_TEXTO_MAS  /* @dataset texto+: create, breakdown, assemble      */
%token <texto> V_SNAP          /* @instantanea: rollback, holds                     */
%token <texto> V_SNAP_TEXTO    /* @instantanea <texto>: hold, release               */
%token <texto> V_SNAP_URL      /* @instantanea <url>: copy                          */

%type <texto> palabra valor_opcion componente_texto

%destructor { free($$); } <texto>

%%

linea
    : /* vacía */                       { res->vacia = 1; }
    | orden opciones
    ;

/* Una producción por FORMA de verbo. El objetivo posicional es SIEMPRE una URL: es lo que
 * permite que una palabra suelta no compita con él. La excepción es `V_CONN`, donde una
 * conexión se nombra por su identificador y no lleva barra. */
orden
    : V_NADA                                   { astVerbo(res, $1); }
    | V_TEXTO texto_opt                        { astVerbo(res, $1); }
    | V_CUALQUIERA url_opt                     { astVerbo(res, $1); }
    | V_CONN conexion_opt                      { astVerbo(res, $1); }
    /* La conexión va por URL —o por `--on`— y no como palabra: aquí la palabra ya es la
     * ranura, y admitir las dos formas volvería a hacer ambiguo `job <id>`. */
    | V_CONN_TEXTO url_opt palabra             { astVerbo(res, $1); astRanura(res, "texto", $3); }
    | V_POOL url_opt                           { astVerbo(res, $1); }
    | V_POOL_FASE url_opt fase_opt             { astVerbo(res, $1); }
    | V_DS url_opt                             { astVerbo(res, $1); }
    | V_DS_TEXTO_OPC url_opt texto_opt         { astVerbo(res, $1); }
    | V_DS_ASIGNA url_opt asignaciones         { astVerbo(res, $1); }
    | V_SNAP url_opt                           { astVerbo(res, $1); }
    | V_SNAP_TEXTO url_opt PALABRA             { astVerbo(res, $1); astRanura(res, "etiqueta", $3); }
    /* --- Y estas NO llevan destino posicional. No es una omisión: su ranura también
     * acepta una URL, así que `clear /dev/sda1` sería a la vez «el pool /dev/sda1» y «el
     * disco sda1 del pool actual». Bison lo reporta como conflicto; la versión escrita a
     * mano elegía en silencio. Para actuar sobre otro sitio, `--on`. */
    | V_POOL_FASE_VDEV fase_opt vdev_opt       { astVerbo(res, $1); }
    | V_POOL_VDEV vdev_opt                     { astVerbo(res, $1); }
    | V_DS_URL URL                             { astVerbo(res, $1); astRanura(res, "destino", $2); }
    | V_DS_RUTA ruta                           { astVerbo(res, $1); }
    | V_DS_TEXTO_MAS textos                    { astVerbo(res, $1); }
    | V_SNAP_URL URL                           { astVerbo(res, $1); astRanura(res, "destino", $2); }
    ;

url_opt
    : /* nada: el sitio actual */
    | URL                                      { astObjetivo(res, $1); }
    ;

/* Una conexión se nombra por su identificador —`oldlau`—, así que aquí sí se admite una
 * palabra. No hay ambigüedad porque estas órdenes no tienen ranuras de palabra. */
conexion_opt
    : /* nada */
    | URL                                      { astObjetivo(res, $1); }
    | PALABRA                                  { astObjetivo(res, $1); }
    ;

fase_opt
    : /* nada */
    | FASE_START                               { astRanura(res, "fase", $1); }
    | FASE_STOP                                { astRanura(res, "fase", $1); }
    | FASE_CANCEL                              { astRanura(res, "fase", $1); }
    | FASE_PAUSE                               { astRanura(res, "fase", $1); }
    | FASE_SUSPEND                             { astRanura(res, "fase", $1); }
    ;

vdev_opt
    : /* nada */
    | PALABRA                                  { astRanura(res, "disco", $1); }
    | URL                                      { astRanura(res, "disco", $1); }
    ;

texto_opt
    : /* nada */
    | palabra                                  { astRanura(res, "texto", $1); }
    ;

/* Una lista de componentes libres: `create <nombre> [prop=valor...]`,
 * `breakdown <directorio> <hijo> ...`. Admite URL y asignaciones porque ahí caben las tres
 * formas: `create @ayer`, `create hijo` y `create hijo compression=lz4`. */
textos
    : componente_texto                         { astRanura(res, "texto", $1); }
    | textos componente_texto                  { astRanura(res, "texto", $2); }
    ;

componente_texto
    : palabra      { $$ = $1; }
    | URL          { $$ = $1; }
    | ASIGNACION   { $$ = $1; }
    ;

ruta
    : URL                                      { astRanura(res, "ruta", $1); }
    | PALABRA                                  { astRanura(res, "ruta", $1); }
    ;

asignaciones
    : ASIGNACION                               { astRanura(res, "props", $1); }
    | asignaciones ASIGNACION                  { astRanura(res, "props", $2); }
    ;

/* Las palabras clave de fase valen también como palabra corriente donde no compiten: un
 * dataset se puede llamar `stop`. */
palabra
    : PALABRA        { $$ = $1; }
    | FASE_START     { $$ = $1; }
    | FASE_STOP      { $$ = $1; }
    | FASE_CANCEL    { $$ = $1; }
    | FASE_PAUSE     { $$ = $1; }
    | FASE_SUSPEND   { $$ = $1; }
    ;
opciones
    : /* nada */
    | opciones OPCION_LARGA                    { astOpcion(res, $2, 0); }
    | opciones OPCION_LARGA valor_opcion       { astOpcion(res, $2, $3); }
    | opciones OPCION_CORTA                    { astBandera(res, $2); }
    ;

valor_opcion
    : PALABRA        { $$ = $1; }
    | URL            { $$ = $1; }
    | ASIGNACION     { $$ = $1; }
    ;

%%

void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg) {
    (void)scanner;
    astError(res, msg);
}

%{
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
%}

/* El tipo va también en la CABECERA generada: la firma de `zfsmcliparse` lo menciona. */
%code requires {
#include "gramatica_ast.h"
}

%define api.pure full
%define api.prefix {zfsmcli}
%lex-param   {void* scanner}
%parse-param {void* scanner} {AnalisisCli* res}
%expect 0

%union {
    char* texto;
}

/* Y estas van DESPUÉS del %union, porque usan su tipo. */
%code provides {
int zfsmclilex(ZFSMCLISTYPE* yylval, void* scanner);
void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg);
}

%token <texto> URL PALABRA ASIGNACION
%token <texto> FASE_START FASE_STOP FASE_CANCEL FASE_PAUSE FASE_SUSPEND
%token CARACTER_MALO

/* Un token por VERBO. El léxico los reconoce con su tabla de palabras clave. */
%token <texto> V_CD V_LS V_PWD V_INFO V_HELP V_EXIT V_FORMAT V_YES V_CLS
%token <texto> V_MASTER_PASSWORD
%token <texto> V_CONNECT V_DISCONNECT V_REFRESH V_EDIT V_DEVICES V_INSTALL_DAEMON
%token <texto> V_JOBS V_JOB V_IMPORT
%token <texto> V_FLUSH V_UPGRADE V_REGUID V_EXPORT V_STATUS V_HISTORY
%token <texto> V_SCRUB V_TRIM V_INITIALIZE V_CLEAR
%token <texto> V_CREATE V_DESTROY V_RENAME V_MOUNT V_UNMOUNT V_PROMOTE
%token <texto> V_GET V_SET V_LOAD_KEY V_UNLOAD_KEY V_CHANGE_KEY
%token <texto> V_SCHEDULE V_SCHEDULES V_LOG V_PEERS V_REPAIR_MOUNTS
%token <texto> V_AUTHORIZE_KEY V_EXPORT_TRUST
%token <texto> V_ROLLBACK V_HOLDS V_HOLD V_RELEASE V_CLONE V_DIFF V_COPY
%token <texto> V_ALLOW V_UNALLOW
%token <texto> V_BREAKDOWN V_ASSEMBLE V_TODIR V_FROMDIR V_RSYNC
%token <texto> V_DESCONOCIDO

%type <texto> palabra componente_texto

%destructor { free($$); } <texto>

%%

linea
    : /* vacía */                        { res->vacia = 1; }
    | orden
    ;

orden
/* --- Navegación --------------------------------------------------------------------- */
    : V_CD destino_opt                       { astVerbo(res, $1); }
    | V_LS destino_opt                       { astVerbo(res, $1); }
    | V_PWD                              { astVerbo(res, $1); }
    | V_INFO destino_opt                     { astVerbo(res, $1); }

/* --- Del intérprete ----------------------------------------------------------------- */
    | V_HELP                             { astVerbo(res, $1); }
    | V_HELP palabra                     { astVerbo(res, $1); astRanura(res, "texto", $2); }
    | V_EXIT                             { astVerbo(res, $1); }
    | V_YES                              { astVerbo(res, $1); }
/* Con argumento TAMBIÉN: el catálogo dice «yes [on|off]» y el intérprete lee esa palabra
 * —`args.front() != "off"`—, pero la producción no la admitía, así que `yes off` moría con
 * la línea de uso y no había forma de volver a activar las confirmaciones sin salir. */
    | V_YES palabra                      { astVerbo(res, $1); astRanura(res, "texto", $2); }
    | V_FORMAT                           { astVerbo(res, $1); }
    | V_FORMAT palabra                   { astVerbo(res, $1); astRanura(res, "texto", $2); }
    | V_CLS                              { astVerbo(res, $1); }
    | V_MASTER_PASSWORD                  { astVerbo(res, $1); }

/* --- Conexiones ---------------------------------------------------------------------
 * Una conexión se nombra por su IDENTIFICADOR —`oldlau`—, que no lleva barra, así que aquí
 * una palabra suelta SÍ puede ser el destino. */
    | V_CONNECT destino_opt             { astVerbo(res, $1); }
    | V_DISCONNECT destino_opt          { astVerbo(res, $1); }
    | V_REFRESH destino_opt             { astVerbo(res, $1); }
    | V_EDIT destino_opt                { astVerbo(res, $1); }
    | V_DEVICES destino_opt             { astVerbo(res, $1); }
    | V_INSTALL_DAEMON destino_opt      { astVerbo(res, $1); }
    | V_JOBS destino_opt                { astVerbo(res, $1); }
/* En estas dos la palabra ya es la ranura, así que la conexión va por URL o por `--on`. */
    | V_JOB url_opt palabra              { astVerbo(res, $1); astRanura(res, "texto", $3); }
    | V_IMPORT url_opt palabra           { astVerbo(res, $1); astRanura(res, "texto", $3); }

/* --- Pools --------------------------------------------------------------------------- */
    | V_FLUSH destino_opt                    { astVerbo(res, $1); }
    | V_UPGRADE destino_opt                  { astVerbo(res, $1); }
    | V_REGUID destino_opt                   { astVerbo(res, $1); }
    | V_EXPORT destino_opt                   { astVerbo(res, $1); }
    | V_STATUS destino_opt                   { astVerbo(res, $1); }
    | V_HISTORY destino_opt                  { astVerbo(res, $1); }
    | V_SCRUB destino_opt fase_opt           { astVerbo(res, $1); }
/* Estas tres NO llevan destino posicional: su ranura también acepta una URL, así que
 * `clear /dev/sda1` sería a la vez «el pool /dev/sda1» y «el disco del pool actual». Bison
 * lo señala como conflicto; la versión escrita a mano elegía en silencio. Va por `--on`. */
    | V_TRIM fase_opt vdev_opt           { astVerbo(res, $1); }
    | V_INITIALIZE fase_opt vdev_opt     { astVerbo(res, $1); }
    | V_CLEAR vdev_opt                   { astVerbo(res, $1); }

/* --- Datasets ------------------------------------------------------------------------ */
    | V_MOUNT destino_opt                    { astVerbo(res, $1); }
    | V_UNMOUNT destino_opt                  { astVerbo(res, $1); }
    | V_PROMOTE destino_opt                  { astVerbo(res, $1); }
    | V_DESTROY destino_opt                  { astVerbo(res, $1); }
    | V_LOAD_KEY destino_opt                 { astVerbo(res, $1); }
    | V_UNLOAD_KEY destino_opt               { astVerbo(res, $1); }
    | V_CHANGE_KEY destino_opt               { astVerbo(res, $1); }
    | V_SCHEDULE destino_opt                 { astVerbo(res, $1); }
    | V_SCHEDULES destino_opt                { astVerbo(res, $1); }
    | V_LOG destino_opt                      { astVerbo(res, $1); }
    | V_PEERS destino_opt                    { astVerbo(res, $1); }
    | V_REPAIR_MOUNTS destino_opt            { astVerbo(res, $1); }
/* Las dos llevan la OTRA máquina como palabra: «authorize-key oldlau» estando en unibody.
 * Por eso su destino propio va por `--on`, como en `copy`: si no, no habría forma de saber
 * cuál de los dos nombres es cuál. */
    | V_AUTHORIZE_KEY palabra                { astVerbo(res, $1); astRanura(res, "texto", $2); }
    | V_EXPORT_TRUST palabra                 { astVerbo(res, $1); astRanura(res, "texto", $2); }
    | V_RENAME url_opt palabra           { astVerbo(res, $1); astRanura(res, "texto", $3); }
    | V_GET url_opt                      { astVerbo(res, $1); }
    | V_GET url_opt palabra              { astVerbo(res, $1); astRanura(res, "propiedad", $3); }
    | V_SET destino_opt asignaciones         { astVerbo(res, $1); }
    | V_CREATE textos                    { astVerbo(res, $1); }
    | V_CLONE textos                     { astVerbo(res, $1); }
    | V_ALLOW textos                     { astVerbo(res, $1); }
    | V_UNALLOW textos                   { astVerbo(res, $1); }

/* --- Instantáneas -------------------------------------------------------------------- */
    | V_ROLLBACK destino_opt                 { astVerbo(res, $1); }
    | V_HOLDS destino_opt                    { astVerbo(res, $1); }
    | V_HOLD url_opt palabra             { astVerbo(res, $1); astRanura(res, "etiqueta", $3); }
    | V_RELEASE url_opt palabra          { astVerbo(res, $1); astRanura(res, "etiqueta", $3); }

/* --- Acciones sobre los DATOS --------------------------------------------------------
 * Llevan una URL o una ruta como argumento, así que el origen va por `--on`/`--from` y no
 * posicionalmente: si no, no habría forma de saber cuál de las dos URL es cuál. */
    | V_COPY URL                         { astVerbo(res, $1); astRanura(res, "destino", $2); }
    | V_RSYNC URL                        { astVerbo(res, $1); astRanura(res, "destino", $2); }
    | V_DIFF URL                         { astVerbo(res, $1); astRanura(res, "destino", $2); }
    | V_TODIR ruta                       { astVerbo(res, $1); }
    | V_FROMDIR ruta                     { astVerbo(res, $1); }
    | V_BREAKDOWN textos                 { astVerbo(res, $1); }
    | V_ASSEMBLE textos                  { astVerbo(res, $1); }

/* Una orden que no está en la tabla del léxico. Se acepta a propósito para que el error que
 * vea el usuario sea «orden desconocida» y no un fallo de sintaxis, que no le diría nada. */
    | V_DESCONOCIDO                      { astVerboDesconocido(res, $1); }
    ;

/* --- Piezas comunes ------------------------------------------------------------------ */

/* El destino, admitiendo un nombre suelto: `cd local`, `mount datos`. Vale en las órdenes
 * que no tienen ranuras de palabra, que son la mayoría. */
destino_opt
    : /* nada: el sitio actual */
    | URL                                { astObjetivo(res, $1); }
    | PALABRA                            { astObjetivo(res, $1); }
    ;

/* El destino solo por URL. Lo usan las SEIS órdenes cuya ranura también es una palabra
 * —`get <prop>`, `hold <etiqueta>`, `rename <nombre>`, `job <id>`, `import <pool>`—: ahí
 * `get compression` sería a la vez «la propiedad compression» y «el dataset compression»,
 * y bison lo señala. Para actuar sobre otro sitio, la URL o `--on`. */
url_opt
    : /* nada: el sitio actual */
    | URL                                { astObjetivo(res, $1); }
    ;

fase_opt
    : /* nada */
    | FASE_START                         { astRanura(res, "fase", $1); }
    | FASE_STOP                          { astRanura(res, "fase", $1); }
    | FASE_CANCEL                        { astRanura(res, "fase", $1); }
    | FASE_PAUSE                         { astRanura(res, "fase", $1); }
    | FASE_SUSPEND                       { astRanura(res, "fase", $1); }
    ;

vdev_opt
    : /* nada */
    | PALABRA                            { astRanura(res, "disco", $1); }
    | URL                                { astRanura(res, "disco", $1); }
    ;


/* Una lista de componentes libres: `create <nombre> [prop=valor...]`,
 * `breakdown <directorio> <hijo> ...`. */
textos
    : componente_texto                   { astRanura(res, "texto", $1); }
    | textos componente_texto            { astRanura(res, "texto", $2); }
    ;

componente_texto
    : palabra      { $$ = $1; }
    | URL          { $$ = $1; }
    | ASIGNACION   { $$ = $1; }
    ;

ruta
    : URL                                { astRanura(res, "ruta", $1); }
    | PALABRA                            { astRanura(res, "ruta", $1); }
    ;

asignaciones
    : ASIGNACION                         { astRanura(res, "props", $1); }
    | asignaciones ASIGNACION            { astRanura(res, "props", $2); }
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


%%

void zfsmclierror(void* scanner, AnalisisCli* res, const char* msg) {
    (void)scanner;
    astError(res, msg);
}

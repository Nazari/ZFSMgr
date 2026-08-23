#pragma once

/* El resultado del análisis: verbo, objetivo, ranuras y opciones.
 *
 * En C liso porque lo rellenan las acciones del yacc. Quien lo consume desde C++ lo copia a
 * sus propias estructuras en la frontera, igual que se hace con el transporte.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define ZFSMCLI_MAX_COMPONENTES 64

typedef struct {
    char* nombre;
    char* valor;
} ZfsmCliPar;

typedef struct {
    int vacia;
    int verboDesconocido; /* el verbo no está en la tabla del léxico */
    char* verbo;
    char* objetivo;                              /* la URL o el id de conexión, o NULL */
    ZfsmCliPar ranuras[ZFSMCLI_MAX_COMPONENTES]; /* nombre de ranura -> valor, en orden */
    int nRanuras;
    ZfsmCliPar opciones[ZFSMCLI_MAX_COMPONENTES]; /* --clave [valor] */
    int nOpciones;
    char* banderas[ZFSMCLI_MAX_COMPONENTES];      /* -r, -rf */
    int nBanderas;
    ZfsmCliPar repetidas[ZFSMCLI_MAX_COMPONENTES]; /* -o p=v, que se repite y ordena */
    int nRepetidas;
    char* error;                                  /* NULL si fue bien */
} AnalisisCli;

void astVerbo(AnalisisCli* r, char* verbo);
void astVerboDesconocido(AnalisisCli* r, char* verbo);
void astObjetivo(AnalisisCli* r, char* url);
void astRanura(AnalisisCli* r, const char* nombre, char* valor);
void astOpcion(AnalisisCli* r, char* nombre, char* valor);
void astBandera(AnalisisCli* r, char* bandera);
void astOpcionRepetida(AnalisisCli* r, char* nombre, char* valor);
void astError(AnalisisCli* r, const char* msg);
void astLibera(AnalisisCli* r);

/* ¿Lleva valor esta opción larga de este verbo? Lo contesta quien conoce el catálogo.
 *
 * El léxico lo necesita porque las OPCIONES no pasan por la gramática: su posición no
 * significa nada —`allow --user u perms` y `allow perms --user u` son la misma orden—, así
 * que meterlas en las producciones obligaba a ponerlas al final y hacía ambiguo distinguir
 * `--user linarese` (opción con valor) de `--everyone linarese` (opción suelta más un
 * argumento). Sabiendo cuáles llevan valor, no hay ambigüedad y valen en cualquier sitio. */
typedef int (*ZfsmCliLlevaValor)(const char* verbo, const char* opcion, void* ctx);

/* Analiza una línea. Devuelve 0 si fue bien. */
int zfsmCliAnaliza(const char* linea, ZfsmCliLlevaValor llevaValor, void* ctx, AnalisisCli* out);

#ifdef __cplusplus
}
#endif

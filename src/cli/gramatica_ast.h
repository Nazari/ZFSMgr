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
    char* error;                                  /* NULL si fue bien */
} AnalisisCli;

void astVerbo(AnalisisCli* r, char* verbo);
void astVerboDesconocido(AnalisisCli* r, char* verbo);
void astObjetivo(AnalisisCli* r, char* url);
void astRanura(AnalisisCli* r, const char* nombre, char* valor);
void astOpcion(AnalisisCli* r, char* nombre, char* valor);
void astBandera(AnalisisCli* r, char* bandera);
void astError(AnalisisCli* r, const char* msg);
void astLibera(AnalisisCli* r);

/* Analiza una línea. Devuelve 0 si fue bien. */
int zfsmCliAnaliza(const char* linea, AnalisisCli* out);

#ifdef __cplusplus
}
#endif

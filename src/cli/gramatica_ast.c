/* El AST y el arranque del analizador. En C, como lo generado. */
#include "gramatica_ast.h"
#include "gramatica.tab.h"

#include <stdlib.h>
#include <string.h>

int zfsmclilex_init(void** scanner);
int zfsmclilex_destroy(void* scanner);
void* zfsmcli_scan_string(const char* s, void* scanner);
void zfsmcli_delete_buffer(void* buf, void* scanner);
int zfsmcliparse(void* scanner, AnalisisCli* res);

static char* dup0(const char* s) {
    char* p = (char*)malloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

void astVerbo(AnalisisCli* r, char* verbo) { r->verbo = verbo; }
void astVerboDesconocido(AnalisisCli* r, char* verbo) {
    r->verbo = verbo;
    r->verboDesconocido = 1;
}
void astObjetivo(AnalisisCli* r, char* url) { r->objetivo = url; }

void astRanura(AnalisisCli* r, const char* nombre, char* valor) {
    if (r->nRanuras >= ZFSMCLI_MAX_COMPONENTES) {
        free(valor);
        return;
    }
    r->ranuras[r->nRanuras].nombre = dup0(nombre);
    r->ranuras[r->nRanuras].valor = valor;
    ++r->nRanuras;
}

void astOpcion(AnalisisCli* r, char* nombre, char* valor) {
    if (r->nOpciones >= ZFSMCLI_MAX_COMPONENTES) {
        free(nombre);
        free(valor);
        return;
    }
    r->opciones[r->nOpciones].nombre = nombre;
    r->opciones[r->nOpciones].valor = valor;  /* puede ser NULL: es una bandera larga */
    ++r->nOpciones;
}

void astBandera(AnalisisCli* r, char* bandera) {
    if (r->nBanderas >= ZFSMCLI_MAX_COMPONENTES) {
        free(bandera);
        return;
    }
    r->banderas[r->nBanderas++] = bandera;
}

void astOpcionRepetida(AnalisisCli* r, char* nombre, char* valor) {
    if (r->nRepetidas >= ZFSMCLI_MAX_COMPONENTES) {
        free(nombre);
        free(valor);
        return;
    }
    r->repetidas[r->nRepetidas].nombre = nombre;
    r->repetidas[r->nRepetidas].valor = valor;
    ++r->nRepetidas;
}

void astError(AnalisisCli* r, const char* msg) {
    if (!r->error) {
        r->error = dup0(msg);
    }
}

void astLibera(AnalisisCli* r) {
    free(r->verbo);
    free(r->objetivo);
    for (int i = 0; i < r->nRanuras; ++i) {
        free(r->ranuras[i].nombre);
        free(r->ranuras[i].valor);
    }
    for (int i = 0; i < r->nOpciones; ++i) {
        free(r->opciones[i].nombre);
        free(r->opciones[i].valor);
    }
    for (int i = 0; i < r->nBanderas; ++i) {
        free(r->banderas[i]);
    }
    for (int i = 0; i < r->nRepetidas; ++i) {
        free(r->repetidas[i].nombre);
        free(r->repetidas[i].valor);
    }
    memset(r, 0, sizeof(*r));
}

/* El léxico necesita saber la clase del PRIMER componente. Se le pasa por el scanner
 * extra, que es lo que flex ofrece para esto sin variables globales. */
typedef struct {
    int arrancado;
} ContextoLex;

void zfsmcliset_extra(void* user, void* scanner);
void* zfsmcliget_extra(void* scanner);

int zfsmCliAnaliza(const char* linea, AnalisisCli* out) {
    memset(out, 0, sizeof(*out));
    void* scanner = NULL;
    if (zfsmclilex_init(&scanner) != 0) {
        astError(out, "no se pudo arrancar el analizador");
        return 1;
    }
    ContextoLex extra;
    extra.arrancado = 0;
    zfsmcliset_extra(&extra, scanner);
    void* buf = zfsmcli_scan_string(linea, scanner);
    const int rc = zfsmcliparse(scanner, out);
    zfsmcli_delete_buffer(buf, scanner);
    zfsmclilex_destroy(scanner);
    if (rc != 0 && !out->error) {
        astError(out, "no se entiende la orden");
    }
    return out->error ? 1 : 0;
}

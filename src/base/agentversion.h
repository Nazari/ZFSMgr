#pragma once

#include <string>

// Las versiones del AGENTE: la que espera este cliente, cómo se ordenan dos, y cuál
// declara un binario que está en disco.
//
// Vive en la capa base porque el intérprete las necesita igual que la interfaz: hoy marca
// con « * » la máquina cuya versión no es la esperada, y al instalar escribe una versión
// en `agent.conf`. Tenía su propia manera de hacer las dos cosas, más pobre —comparaba
// cadenas con `!=` y escribía la versión con la que se compiló él, no la del binario que
// estaba copiando—, y esa es exactamente la clase de divergencia que la capa base existe
// para evitar.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::agentversion {

// La que este cliente espera. Del compilador: lleva el sufijo del marcador de esquema.
std::string laEsperada();

// La versión del PROTOCOLO, que no es la del agente y cambia mucho menos.
std::string apiEsperada();

// Ordena dos versiones «may.men.par[rcN][.sufijo]». Devuelve <0, 0 o >0.
//
// Un candidato a release —«0.93.0rc1»— va ANTES que su final. Lo que no encaje en la
// forma se compara como texto, que no es correcto pero es predecible.
int compara(const std::string& a, const std::string& b);

// La versión que declara un binario de agente, leyéndola del FICHERO.
//
// Hace falta leerla así porque el agente empaquetado suele ser de otra plataforma y no se
// puede ejecutar aquí para preguntárselo.
//
// **Busca cualquier versión bien formada, no la de esta compilación.** Antes se anclaba al
// prefijo de la versión actual, y por eso NO encontraba nada justo en el caso que
// importa: un agente empaquetado de otra versión —que es cuando hay algo que avisar—.
std::string versionEnBinario(const std::string& ruta);

}  // namespace zfsmgr::base::agentversion

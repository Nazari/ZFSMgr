#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Saca del código del CLI los pares {clave, castellano} y los vuelca a i18n/es.json.

La fuente de verdad es EL CÓDIGO, no un fichero acumulado a mano: un catálogo que se
mantiene aparte se llena de claves que ya no usa nadie y de textos que dejaron de
coincidir. Aquí se lee lo que hay y se dice qué sobra.

Reconoce las dos formas en que se marca un texto traducible:
    T("clave", "castellano")  /  TC("clave", "castellano")
    {"clave", "castellano"}   (el catálogo de ayuda)
"""
import json, re, sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[1]

def cadenasC(texto, pos):
    """Lee una cadena C en `pos`, uniendo las adyacentes que el compilador concatenaría.
    Devuelve (valor, posicion_siguiente) o (None, pos)."""
    n = len(texto)
    if pos >= n or texto[pos] != '"':
        return None, pos
    partes = []
    k = pos
    while k < n and texto[k] == '"':
        k += 1
        buf = ''
        while k < n and texto[k] != '"':
            if texto[k] == '\\':
                buf += texto[k]; k += 1
                if k < n:
                    buf += texto[k]; k += 1
                continue
            buf += texto[k]; k += 1
        partes.append(buf)
        k += 1
        m = k
        while m < n and texto[m] in ' \t\n':
            m += 1
        if m < n and texto[m] == '"':
            k = m
        else:
            break
    return ''.join(partes), k

def desescapa(s):
    return (s.replace('\\n', '\n').replace('\\t', '\t')
             .replace('\\"', '"').replace("\\'", "'").replace('\\\\', '\\'))

def extrae(ruta):
    texto = ruta.read_text(encoding='utf-8')
    fuera = {}
    for m in re.finditer(r'(?:\bTC?\(|\{)\s*"(t_[A-Za-z0-9_]+)"\s*,\s*', texto):
        valor, _ = cadenasC(texto, m.end())
        if valor is not None:
            fuera[m.group(1)] = desescapa(valor)
    return fuera

def main():
    encontrados = {}
    for ruta in sorted((RAIZ / 'src' / 'cli').glob('*.cpp')):
        encontrados.update(extrae(ruta))
    esRuta = RAIZ / 'i18n' / 'es.json'
    es = json.loads(esRuta.read_text(encoding='utf-8'))
    tr = es.setdefault('translations', {})
    nuevas = 0
    for k, v in encontrados.items():
        if tr.get(k) != v:
            tr[k] = v
            nuevas += 1
    esRuta.write_text(json.dumps(es, ensure_ascii=False, indent=2, sort_keys=True) + '\n',
                      encoding='utf-8')
    # Lo que está en el catálogo inglés y ya no usa nadie: se avisa, no se borra. Puede ser
    # de la interfaz gráfica, que comparte fichero.
    enRuta = RAIZ / 'i18n' / 'en.json'
    en = json.loads(enRuta.read_text(encoding='utf-8'))['translations']
    sinTraducir = [k for k in encontrados if k not in en]
    print(f"claves en el código: {len(encontrados)}")
    print(f"actualizadas en es.json: {nuevas}")
    print(f"sin traducir al inglés: {len(sinTraducir)}")
    for k in sorted(sinTraducir, key=lambda x: encontrados[x]):
        print("   %-22s %s" % (k, repr(encontrados[k])[:80]))
    return 0

if __name__ == '__main__':
    sys.exit(main())

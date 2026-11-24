# Una sola configuración para toda la red

Estado: **implementado** (fases 1–3), pendiente de ponerlo en marcha en la red real. Decisiones del usuario del 2026-11-24:

- La configuración es de la RED, no de cada máquina: la misma visión desde cualquier ordenador.
- Los secretos, cifrados, se guardan en todos los daemons.
- Una sola contraseña maestra para toda la red.

## El problema

Hoy cada ordenador tiene su `~/.config/ZFSMgr/config.json` y su `trust-store.json`:

- fc16 ve 6 máquinas, MMEla 3, a1278 2; unib tiene una configuración antigua sin migrar.
- Las llaves FIDO2 están registradas solo en fc16: en MMEla no sirven.
- Una dirección que cambia (FreeB, de la red de libvirt a Tailscale) se arregla a mano en cada
  ordenador y luego se reparte a los daemons (`peers --push`).
- Cada configuración tiene su propia contraseña maestra y su propia clave de datos.

## La idea

La configuración de la red es UN documento que guardan **todos los daemons** —ya se conocen entre
sí y ya guardan las programaciones—. El cliente (CLI o interfaz) lo lee de cualquiera al que
llegue, guarda una copia local y escribe los cambios en uno, que los propaga a los demás.

## Qué es de la red y qué es de cada ordenador

### Documento de red (`network.json`, igual en todas partes)

```json
{
  "schema": 1,
  "revision": 42,
  "written_by": "<uid de la máquina del cliente que escribió>",
  "written_at": 1795520000,
  "master": { "slots": [ … ], "salt": "…", "check": "…" },
  "machines": [
    {
      "uid": "c5aa63e237974d848427b62b044ae5a5",
      "name": "unib",
      "os": "Linux",
      "addresses": ["unib", "100.99.167.18"],
      "ssh": { "user": "linarese", "port": 22, "family": "auto" },
      "sudo": true,
      "secrets": { "password": "<cifrado con la clave de datos>" },
      "tls": { "server_cert": "…", "client_cert": "…", "client_key": "<cifrado>" }
    }
  ],
  "mac": "<HMAC del documento con una clave derivada de la clave de datos>"
}
```

- **Identidad = `uid`** de la máquina (el que ya lee cada daemon de sí mismo), no el nombre: el
  nombre es una etiqueta que se puede cambiar.
- **`master`**: el bloque de ranuras de hoy (`storefiles.h`, «THE DATA KEY»): la clave de datos
  envuelta una vez por la contraseña maestra y una vez por cada llave FIDO2 registrada. Al ser de
  la red, **una YubiKey registrada una vez abre en todos los ordenadores**.
- **`addresses`**: varias, en orden. Cada cliente usa la primera que le responde: desde fc16,
  FreeB es `192.168.122.119`; desde MMEla, `freeb`. Se acaba «arreglar la dirección en un sitio y
  repartirla».
- **Secretos** (contraseña SSH/sudo, clave privada TLS de cliente): cifrados con la clave de datos,
  como hoy. Un daemon guarda un bloque que **no puede leer**: sin la maestra o una llave, root en
  cualquier máquina solo ve texto cifrado.

### Local a cada ordenador (`~/.config/ZFSMgr/local.json`)

- **La copia en caché** del último documento de red visto (para arrancar sin red y deprisa).
- **Rutas de claves SSH** por máquina (dependen del sistema y del usuario de este ordenador).
- **Dirección preferida** por máquina, si se quiere forzar una (si no, la primera que responde).
- **Preferencias de la interfaz**: idioma, modo avanzado, árbol expandido…
- **«Esta máquina»** ya no es una conexión aparte: es la de la red cuyo `uid` coincide con el de
  este ordenador (o ninguna, en un portátil sin ZFS: el modo cliente de la fase A).

## Integridad: los daemons guardan, pero no pueden falsificar

Que un daemon no pueda LEER los secretos no basta: root en una máquina podría MODIFICAR el
documento —cambiar la dirección de otra máquina por la suya para recoger la contraseña SSH que el
cliente le mande—. Por eso:

- El documento lleva un **HMAC** con una clave derivada de la clave de datos (HKDF, etiqueta
  propia). Solo quien abre la red —con la maestra o una llave— puede escribir un documento válido.
- El cliente **verifica el HMAC** al leerlo y **rechaza** uno que no verifica, diciendo de qué
  daemon vino.
- **Sin vuelta atrás**: el cliente rechaza una revisión MENOR que la de su copia (un daemon que
  sirva un documento viejo, válido pero superado). El HMAC cubre la revisión.

Los daemons validan solo la forma (JSON, tamaño, revisión mayor que la suya): no tienen la clave
para más.

## Guardar y propagar

### En el daemon

- Fichero `/etc/zfsmgr/network.json` (Windows: `C:\ProgramData\ZFSMgr\agent\network.json`),
  solo root, escritura atómica.
- Verbos nuevos (`serve.cpp`, tabla única):
  - `--net-get` → el documento, o vacío si no tiene.
  - `--net-put <revisión-base> <documento-base64>` → lo acepta solo si `revisión-base` es la que
    tiene (comparar-e-intercambiar) y la nueva es mayor; si no, `net_conflict` con la suya.
- Al aceptar uno nuevo, lo **empuja a los demás daemons** con las llaves que ya tienen (las de
  `peers`). Al arrancar, y cada pocos minutos, **pregunta la revisión** a los demás y se trae la
  mayor. Una máquina que estuvo apagada se pone al día sola.

### En el cliente

- Al arrancar: la copia local, y en segundo plano `--net-get` a las máquinas a las que llega; si
  alguna tiene una revisión mayor y su HMAC verifica, se adopta y se guarda.
- Al cambiar algo (añadir máquina, cambiar dirección, contraseña de sudo, registrar una llave):
  se escribe en UN daemon con `--net-put` sobre la revisión que se tenía. Si otro escribió antes
  (`net_conflict`), se trae la nueva, se vuelve a aplicar el cambio y se reintenta: dos cambios a
  la vez no se pisan.

## Altas

- **La primera máquina de la red**: el primer `create` sobre una configuración vacía crea la red
  (clave de datos nueva, contraseña maestra) y, en cuanto su daemon está instalado, le escribe el
  documento.
- **Añadir una máquina**: como hoy (SSH, instalar daemon) + el documento nuevo se escribe y se
  propaga + las llaves (`Keys`, ya automáticas en la interfaz).
- **Un ordenador nuevo** (p. ej. un portátil sin ZFS): necesita llegar a UN daemon y la contraseña
  maestra o una llave. Hoy eso exige SSH a esa máquina; con la **fase B** (TLS directo con
  certificado de cliente propio) bastará con autorizarlo desde un ordenador que ya esté dentro.

## Migración (una vez; sin compatibilidad hacia atrás)

Decisión del usuario (2026-11-24): **la configuración válida es la de fc16** para toda la red. No
hay asistente ni mezcla: el documento de red se construye a partir de `config.json` y
`trust-store.json` de fc16 —los secretos ya están cifrados con su clave de datos y pasan tal
cual—, se escribe en todos los daemons, y los demás ordenadores la adoptan con la contraseña
maestra de fc16 (o sus llaves FIDO2), dejando solo su `local.json`.

## Fases

1. **Modelo y cifrado** (capa base, sin red): el documento, su HMAC, la regla de revisiones, la
   conversión desde `config.json` + `trust-store.json`. Pruebas en `base_test`. Comprobar aquí
   que una ranura FIDO2 registrada en un ordenador abre en otro (en Windows la credencial es
   residente: verificarlo en a1278).
2. **Daemon**: `network.json`, `--net-get`/`--net-put`, propagación y puesta al día. Batería
   contra los daemons de prueba.
3. **Cliente**: CLI e interfaz leen la red; la copia local; escribir con reintento por conflicto;
   las direcciones múltiples. `--serve` lo expone a la interfaz sin cambiar lo demás del
   protocolo.
4. **Migración**: sembrar la red con la configuración de fc16 y que los demás la adopten.

## Cómo se engancha en el cliente (fase 3)

Decisión del usuario (2026-11-24): **`zfsm://local` desaparece**; cada máquina, también la
propia, se llama por su nombre de red.

- **3a (hecha, 4381137)**: `network::toClientConfig` da el documento con la forma de
  `config.json` + `trust-store.json`; la máquina cuyo `uid` es el de este ordenador
  (`osinfo::machineUuid`, el mismo código que usa el daemon) es la LOCAL, con su nombre de red.
- **3b (hecha, c76e19b)**: en modo red, el directorio que leen y escriben `storefiles`/`session` es una
  **materialización** del documento en `~/.config/ZFSMgr/network/` (`config.json`,
  `trust-store.json`, el documento firmado `network.json` y `local.json`). Leer no cambia.
  Escribir (crear/editar/quitar máquina, TLS negociado, uid, maestra, llaves) escribe ahí como
  hoy y después **publica**: se trae la última revisión de un daemon, se **fusiona** lo cambiado
  —por `uid`; las direcciones de la red se conservan y una dirección editada pasa a ser la
  primera; la dirección preferida de `local.json` no es un cambio de la red—, se sube la revisión,
  se firma y `--net-put`; con `net_conflict`, se repite. Al arrancar, tras abrir la maestra, se
  pregunta a los daemons por una revisión mayor (`judge`) y se rematerializa.
  - Una máquina recién creada sin `uid` (sin daemon todavía) queda en `local.json` como
    pendiente hasta que su daemon dé el `uid`.
- **3c (hecha, c76e19b)**: `network seed` (fc16 sube su configuración) y `network join` (los demás descargan,
  abren con la maestra de la red y conservan solo `local.json`: rutas de claves por `uid`).
- **3d (hecha, 37fec34)**: sin `local`: la conexión de este ordenador se llama como él también
  fuera de la red; las interfaces la buscan por su tipo; la batería averigua su nombre al empezar.
- **3e (hecha, 37fec34)**: `edit --addresses a,b,…`; al arrancar, cada ordenador prueba las
  direcciones de cada máquina en orden y guarda en `local.json` la primera que le responde.

## Riesgos y preguntas abiertas

- **Todo depende de una contraseña**: quien la tenga abre la red entera (antes, solo un
  ordenador). Mitigación: llaves FIDO2 para el uso diario; la contraseña, guardada aparte.
- **Ranuras FIDO2 en Windows**: exigen credencial residente; hay que comprobar que la misma
  ranura sirve en los cuatro sistemas.
- **Tamaño**: con muchas máquinas el documento crece (certificados); se puede separar el TLS en
  otro documento si hiciera falta.
- **Un daemon con el reloj mal**: no afecta: manda la revisión, no la hora.

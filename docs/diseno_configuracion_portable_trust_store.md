# Diseño técnico: configuración portable y trust-store

## Objetivo

Permitir que una instalación de ZFSMgr pueda transferir a otra máquina la parte portable de su configuración: conexiones conocidas y material TLS del daemon cifrado con el password maestro. La máquina destino podrá ejecutar ZFSMgr y reutilizar esas conexiones sin volver a extraer `client.key` de cada daemon remoto.

La configuración local de UI, caches y estado transitorio no debe viajar entre máquinas.

## Problema actual

`config.json` contiene hoy datos de naturaleza distinta:

- configuración local de la instalación,
- perfiles de conexión,
- credenciales cifradas,
- certificados TLS del daemon por conexión,
- estado visual y preferencias de UI,
- datos derivados o cacheados.

Copiar `config.json` completo al crear una conexión remota puede funcionar en casos simples, pero introduce riesgos:

- la conexión `Local` del origen no representa la máquina destino,
- rutas locales, tamaños de ventana, columnas y estado expandido no son portables,
- caches de árbol pueden quedar obsoletas o pertenecer a otro host,
- se amplía la exposición de secretos si se copia más información de la necesaria,
- una fusión posterior entre configuraciones puede sobrescribir cambios locales.

## Diseño propuesto

Separar el modelo en dos ficheros:

- `config.json`: configuración local de esta instalación.
- `trust-store.json`: información portable de conexiones y confianza, cifrada con el password maestro cuando contenga secretos.

Ubicación inicial:

- Linux/FreeBSD/macOS: `~/.config/ZFSMgr/trust-store.json`
- Windows: mismo directorio de configuración que use `ConnectionStore::configDir()`

El primer paso puede mantener compatibilidad escribiendo también los campos TLS actuales en `config.json`. La migración completa debe permitir que `ConnectionStore` lea ambos sitios y prefiera `trust-store.json` cuando exista.

## Contenido de `trust-store.json`

Formato recomendado:

```json
{
  "schema": 1,
  "created_by": "ZFSMgr",
  "connections": [
    {
      "id": "mbp",
      "name": "MBP",
      "machine_uid": "...",
      "conn_type": "SSH",
      "os_type": "macOS",
      "host": "mbp.tailnet.example",
      "port": 22,
      "ssh_address_family": "auto",
      "username": "encv1:...",
      "key_path": "",
      "use_sudo": true,
      "daemon_tls_port": 47653,
      "daemon_tls_server_cert_pem": "encv1:...",
      "daemon_tls_client_cert_pem": "encv1:...",
      "daemon_tls_client_key_pem": "encv1:..."
    }
  ]
}
```

Campos incluidos:

- identificador, nombre y endpoint de conexión,
- tipo de conexión y SO detectado,
- usuario cifrado si procede,
- password cifrado si se decide exportarlo explícitamente,
- configuración mínima de sudo,
- puerto TLS del daemon,
- certificado servidor del daemon,
- certificado cliente,
- clave cliente cifrada.

Campos excluidos:

- conexión `Local` del equipo origen,
- cache de árbol,
- estado expandido/colapsado,
- tamaño de columnas,
- selección origen/destino,
- logs,
- rutas locales de ficheros,
- estado de jobs o transferencias,
- cualquier dato calculado desde refrescos anteriores.

## Reglas de cifrado

Todos los secretos deben permanecer cifrados con `SecretCipher` y el password maestro:

- `password`,
- `username` si ya se guarda cifrado,
- `daemon_tls_client_key_pem`,
- opcionalmente `daemon_tls_client_cert_pem` y `daemon_tls_server_cert_pem` para mantener homogeneidad con el almacenamiento actual.

La máquina destino solo podrá usar el trust-store si el usuario desbloquea ZFSMgr con el mismo password maestro, o si se implementa una operación explícita de re-cifrado durante la importación.

## Exportación hacia una conexión remota

Nueva acción propuesta:

`Exportar configuración portable a esta conexión`

Flujo:

1. Construir un `trust-store.json` temporal con las conexiones exportables.
2. Excluir siempre la conexión `Local` del equipo origen.
3. Cifrar cualquier campo sensible que aún esté en claro.
4. Transferir por SSH al usuario remoto:
   - `~/.config/ZFSMgr/trust-store.import.json`
5. En la máquina remota, si existe ZFSMgr instalado, fusionar opcionalmente:
   - `trust-store.import.json` -> `trust-store.json`
6. No sobrescribir `config.json` remoto completo.

La acción debe ser opt-in. Al usuario se le debe informar de que la máquina destino podrá conectarse a los daemons remotos con el mismo password maestro.

## Importación y fusión

Reglas de fusión:

- clave primaria: `machine_uid` si existe; si no, `id`; si no, `name + host + port + username`,
- no sobrescribir campos locales no vacíos salvo confirmación,
- si endpoint cambia, invalidar material TLS asociado,
- si endpoint es estable, preservar TLS existente,
- si el mismo daemon aparece con certificado servidor distinto, pedir confirmación o marcar como atención,
- nunca importar la conexión `Local` del origen como `LOCAL`; si interesa, crearla como conexión SSH normal solo con confirmación explícita.

Estados de conflicto:

- `new`: conexión nueva,
- `same`: sin cambios relevantes,
- `updated`: se actualizan campos no sensibles,
- `tls-rotated`: cambia certificado/clave TLS,
- `conflict`: requiere decisión del usuario.

## Bootstrap de daemon y TLS

Cuando se instala o actualiza el daemon:

1. ZFSMgr obtiene `server.crt`, `client.crt` y `client.key` desde el remoto.
2. Persiste el material en el trust-store local cifrado con password maestro.
3. Elimina `client.key` del remoto cuando el agente ya no la necesita allí.
4. Actualiza la cache en memoria de la sesión.

Si `trust-store.json` existe, esta persistencia debe ir ahí. Mientras dure la transición, puede duplicarse en `config.json` para compatibilidad.

## Cambios de código sugeridos

1. Añadir una clase pequeña `TrustStore`.
2. Mover los campos TLS persistentes fuera de `ConnectionProfile` o mantenerlos como vista compuesta cargada desde `ConnectionStore + TrustStore`.
3. Hacer que `ConnectionStore::loadConnections()` fusione:
   - conexiones de `config.json`,
   - material portable de `trust-store.json`.
4. Hacer que `persistRemoteDaemonTlsMaterial()` escriba en `trust-store.json`.
5. Añadir export/import explícito desde menú contextual de conexión.
6. Añadir migración:
   - si `config.json` contiene TLS y no existe entrada en `trust-store.json`, copiarla al trust-store.
7. Añadir tests/manual checks para:
   - importación con mismo password maestro,
   - importación con password maestro distinto,
   - cambio de endpoint,
   - rotación de certificado servidor,
   - exclusión de `Local`.

## Compatibilidad

Durante una fase de transición:

- lectura: `trust-store.json` primero, `config.json` como fallback,
- escritura: `trust-store.json` como destino principal, `config.json` opcional como legacy,
- UI: no debe cambiar la experiencia del usuario salvo nuevas acciones de export/import.

Cuando la migración esté estabilizada, `config.json` debe dejar de almacenar `daemon_tls_*`.

## Estado de implementación

Implementado en la primera fase:

- `ConnectionStore` expone `trustStorePath()` y usa `~/.config/ZFSMgr/trust-store.json`.
- Las nuevas escrituras de material `daemon_tls_*` se persisten en `trust-store.json`.
- `config.json` deja de ser el destino principal de `daemon_tls_*`.
- La carga de conexiones fusiona automáticamente el material TLS desde `trust-store.json`.
- Las instalaciones antiguas con `daemon_tls_*` todavía dentro de `config.json` se migran al trust-store durante la carga.
- La validación del password maestro incluye secretos cifrados en `trust-store.json`.
- La rotación del password maestro re-cifra también los campos TLS del trust-store.
- Si cambia el endpoint de una conexión existente, se invalida su entrada en el trust-store para no reutilizar certificados de otro host.
- Menú contextual de conexión: `Exportar trust-store a esta conexión`.
  - Copia solo `trust-store.json`.
  - No toca `config.json` remoto.
  - Crea backup del trust-store remoto anterior si existe.
  - Usa stdin del canal remoto para no incrustar secretos en la línea de comando.

Pendiente:

- diálogo de importación/fusión con resolución de conflictos,
- eliminación definitiva de lectura legacy desde `config.json` cuando haya pasado la fase de migración.

## Decisión de diseño

No copiar `config.json` completo entre máquinas.

Sí copiar un trust-store portable, limitado y cifrado, con importación/fusión controlada. Esto conserva la comodidad operativa sin mezclar identidad local, estado visual y secretos de confianza en un único fichero transportado a ciegas.

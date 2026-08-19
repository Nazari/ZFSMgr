# Servidor web: la interfaz sin Qt

## Por qué

La interfaz de Qt son **50.945 líneas**, `zfsmgr_qt` pesa **9,3 MB** y el `.app` de macOS
**79 MB**. Enfrente, el agente ocupa **545 KB** y el intérprete **1,1 MB**. Casi toda esa
diferencia es Qt, y con él vienen los 18,6 GB de imagen de toolchain, el aprovisionamiento
por objetivo y las cuatro pasadas de una release.

Esto no era planteable hasta hace unos días. Ahora sí, y por una razón concreta: **la capa
base son 9.016 líneas sin Qt** y `zfsmgr_cli` ya demuestra que sobre ella se sostiene un
cliente completo. El servidor web no inventa una capa nueva — es el mismo cliente con una
cara HTTP en vez de un terminal.

**Este documento fija dos decisiones y una lista.** Sin ellas, «lo más parecido posible a
la GUI» no se puede dar por terminado nunca.

## Decisión 1: artefacto APARTE, no dentro del daemon

`zfsmgr-web` es un binario nuevo, hermano del intérprete, que corre **como el usuario**.
El daemon **no se toca**.

```
zfsmgr-web  (tu usuario)
    │  lee ~/.config/ZFSMgr        igual que la GUI y el cli
    │  sirve HTTPS al navegador
    └──> zfsmgr_base ──> túnel SSH + mTLS ──> daemon (root, sin cambios)
```

Tres hechos, medidos en el código, la sostienen:

**El daemon corre como root y no puede no hacerlo.** Su unidad de systemd no lleva `User=`
—`ExecStart=/usr/local/libexec/zfsmgr-agent --serve` y nada más—, porque ejecuta `zfs` y
lee el material TLS de `/etc/zfsmgr`. Un servidor HTTP con sesiones y plantillas dentro de
ese proceso convierte cualquier fallo suyo en ejecución como root. Hoy su superficie es un
socket en `127.0.0.1` con mTLS y 82 verbos tipados que acaban en `execvp`; es pequeña a
propósito.

**El daemon no sabe qué es una «conexión».** Cero menciones a la contraseña maestra en sus
8.501 líneas, y las cuatro de «connections» son comentarios sobre un fichero ya borrado. El
daemon es de UNA máquina. Quien conoce la lista de máquinas, el almacén de confianza y la
clave maestra es el cliente, y una interfaz web es cliente por definición: enseña varias
máquinas a la vez. Metida en el daemon, habría que contestar «¿en cuál de los cuatro vive
la configuración del usuario?», y no hay buena respuesta.

**El despliegue dejaría de ser simétrico.** El agente viaja a cada máquina y se actualiza
solo; su versión sale del SHA de la lista de verbos, así que tocar el servidor obligaría a
recompilar y reinstalar los cuatro agentes. El servidor web es algo que uno arranca donde
está. Aparte, se puede escribir y tirar tres veces sin recompilar un solo agente.

## Decisión 2: el modelo de seguridad, antes que el código

Un servidor que corre como su usuario, con la clave maestra en memoria y un puerto
escuchando, es una llave de todas sus máquinas. Esto se decide ahora:

| | Decisión | Por qué |
|---|---|---|
| Escucha | **`127.0.0.1` por omisión** | Lo mismo que hace el daemon. Exponerlo es una decisión explícita, nunca el valor de fábrica. |
| Desde fuera | **túnel SSH** | Es el mecanismo que el programa ya usa para todo. No se inventa otro. |
| Clave maestra | **se pide al arrancar**, vive en memoria del proceso | Es exactamente lo que hace hoy la GUI. La alternativa —por sesión de navegador— obliga a sostener sesiones y expirarlas, y eso es un modelo nuevo. |
| Sesión | cookie de sesión con marca de tiempo, `Secure`, `HttpOnly`, `SameSite=Strict` | |
| Mutaciones | token anti-CSRF por sesión | El navegador manda peticiones que el terminal no manda: una página cualquiera puede hacer POST a `localhost`. |
| TLS | certificado propio en `~/.config/ZFSMgr/web/` | Sin él, la cookie viaja en claro aunque sea por bucle local. |

**Lo que NO se hace en la primera versión, y conviene que se vea:** ni usuarios múltiples,
ni permisos por usuario, ni exposición a Internet. Un servidor local para el dueño de la
máquina. Añadir lo otro después es posible; darlo por hecho al principio es la forma de
acabar con un modelo de seguridad a medias.

## Decisión 3: WebDAV en el mismo servidor

Los exploradores de archivos no necesitan un plugin nativo por plataforma: necesitan un
protocolo que ya monten. WebDAV es HTTP con seis verbos más, así que **cabe en el mismo
servidor y en la misma escucha**.

| Explorador | Cómo se llega | Coste |
|---|---|---|
| Finder | *Ir → Conectar al servidor* → `https://…` | ninguno |
| Explorer | *Conectar a unidad de red* | ninguno |
| Dolphin | `webdav://` nativo | ninguno |

Lo que se pierde: el esquema literal `zfsm://`. Lo que se gana: funciona en los tres, sin
firma de Apple —que el proyecto no paga y que File Provider y FSKit exigen—, sin una DLL en
proceso dentro de Explorer y sin escribir dos veces el mismo backend para KIO y GVfs.

El modelo de datos ya existe: `#content` lee el contenido de un dataset y el de una
instantánea por `.zfs/snapshot`. Un KIO worker para Dolphin queda como algo posterior y
opcional, que es el único de los tres nativos que sale barato.

## La lista de paridad

«Parecido a la GUI» no se puede verificar. Esto sí. La columna *cli* dice si esa función ya
está resuelta en la capa base —y por tanto es fachada— o si hay que escribirla.

**Ya en la capa base (fachada HTML, sin lógica nueva):** listar conexiones, pools y
datasets; propiedades y su edición; instantáneas —crear, borrar, revertir, retener,
clonar—; permisos delegados; crear y destruir pools y datasets; importar y exportar;
scrub, trim, initialize; montar y desmontar; cifrado —cargar, descargar y cambiar clave—;
copiar y nivelar entre máquinas; desglosar y ensamblar; instalar el daemon; programación
GSA; registro del daemon; trabajos en segundo plano.

**Hay que escribirlo:** el árbol navegable con estado (expandido, selección), la edición en
línea de propiedades, la lista de pendientes como plan de trabajo, el diálogo de creación
de pool con sus dispositivos, y las capturas de ayuda.

**Se decide si entra:** los tres idiomas —el catálogo ya está en `i18n/*.json` y la capa
base lo lee— y el tema oscuro.

## Orden de trabajo

| Fase | Qué | Por qué antes |
|---|---|---|
| 0 | **HECHA (9533fae)** — HTTP/1.1 sobre OpenSSL + sesión + CSRF, sirviendo *una* página con la lista de conexiones | Es donde vive el riesgo. Si esto no queda bien, lo demás no importa. |
| 1 | **HECHA** — Lectura completa: pools, datasets, propiedades, instantáneas | Todo son verbos `--dump-*`, que son inocuos y verificables uno a uno |
| 2 | **HECHA** — Mutaciones, con confirmación | Aquí empieza a poder romper cosas |
| 3 | WebDAV sobre la misma escucha | Cae casi solo una vez hay servidor |
| 4 | Paridad del resto de la lista | |
| 5 | Retirar Qt | Solo cuando 4 esté medido contra la lista, no antes |

## Lo que hay que resolver y todavía no sé

**El tamaño real, ya medido en la fase 0:** el binario son 329 KB frente a los 9,3 MB de
`zfsmgr_qt`. No dice cuánto ocupará con la interfaz entera, pero sí que el punto de partida
no arrastra peso.

**El túnel se monta en UN hilo.** `TransportSession` tiene `tunnelsAllowedHere` y
`runWhereTunnelsAllowed` porque en la GUI el montaje se ordena al hilo de interfaz. Un
servidor atiende varias peticiones a la vez; hay que decidir si se serializa el montaje en
un hilo propio —lo más parecido a lo que hay— o si se hace reentrante, que es más trabajo y
más riesgo.

*Estado tras la fase 1:* todavía no ha aparecido, porque el servidor atiende de una en una
y el túnel se monta en su mismo hilo, igual que en el intérprete. Aparecerá el día que se
atienda en paralelo — y ese día, no antes, hay que decidirlo.

**Cuánto se parece de verdad.** De las 50.945 líneas de Qt, una parte grande es el árbol y
la edición en línea, que en un navegador salen mucho más baratas. No me atrevo a dar un
número de cuánto encoge hasta tener la fase 1 hecha y medida.

**Qué pasa con las capturas de la ayuda.** Se generan hoy con `ui_doc_capture`, que es Qt.
O se rehacen contra el servidor, o la ayuda se queda con las de la interfaz vieja.

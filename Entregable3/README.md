# DSM: memoria compartida distribuida (prototipo 2)

IC-6600 Principios de Sistemas Operativos, II Semestre 2026.
Rodrigo Sebastián Donoso Chaves y Emily Sánchez.

Cada nodo ejecuta un demonio `dsmd`. Las aplicaciones se enlazan con `libdsm.a`,
reservan memoria con `dsm_malloc` y la usan con punteros normales; los fallos de
página (SIGSEGV) se resuelven pidiendo la página por la red con el protocolo MSI.

Novedades del prototipo 2:

- `dsm_free`, que libera el segmento en todos los nodos y devuelve su capacidad.
  Si un hogar rechaza la creación de un segmento, la creación se deshace.
- Desalojo de copias de páginas de otros hogares al pasar de `cache_remota`,
  con FIFO o con reloj (segunda oportunidad). Las páginas en M vuelven a su
  hogar con WRITEBACK.
- Candados distribuidos (`dsm_candado`, `dsm_soltar`).
- Consultas globales (`dsmctl -g`) y monitor web en el navegador.
- Ingreso por cualquier semilla, que redirige al coordinador si no lo es.
  Estado de sospechoso antes de dar un nodo por caído.
- Aplicación 2: procesamiento de un video completo cargado en el DSM.

## Compilar

Requiere Linux x86-64 (kernel 4.17 o más nuevo), gcc y make. Para preparar el
video también se necesita ffmpeg.

    make            # dsmd, dsmctl, libdsm.a y las apps de prueba
    make clean

## Configurar

Todos los nodos usan el mismo archivo `dsm.conf` (hay uno de ejemplo en `conf/`).
Si un nodo tiene un archivo distinto, su ingreso se rechaza.

| Clave | Ejemplo | Para qué |
| --- | --- | --- |
| `memoria_por_nodo` | 64M | Memoria que aporta cada nodo como hogar |
| `cache_remota` | 64M | Copias de páginas de otros hogares antes de desalojar |
| `reemplazo` | reloj | `fifo` o `reloj` |
| `puerto_minimo` / `puerto_maximo` | 27500 / 27599 | Rango de la pareja; el nodo k usa `27500 + 2k` (datos) y `27500 + 2k + 1` (monitor web) |
| `semilla` | 127.0.0.1:27500 | Nodos a los que se pide el ingreso. Se puede repetir; sirve cualquier nodo del clúster |
| `sospecha_tras` / `caida_tras` | 1500 / 3000 | ms sin mensajes para marcar un nodo como sospechoso o como caído |

En el laboratorio, `semilla` debe llevar la IP real de una máquina del clúster
(por ejemplo, la del nodo 0) y el mismo archivo se copia a todas las máquinas.

## Ejecutar

En la máquina del nodo fundador:

    cp conf/dsm.conf .
    ./dsmd -f

En cada máquina (o terminal) adicional, con el mismo `dsm.conf`:

    ./dsmd

Cada demonio imprime su número de nodo. Las aplicaciones se conectan al demonio
indicado en la variable `DSM_NODO` (por defecto 0). Todas reciben `-p` (procesos
en total) y `-r` (número de este proceso); el proceso 0 crea los segmentos y,
al final, verifica el resultado y los libera.

### Aplicación 1: multiplicación de matrices

    DSM_NODO=0 ./apps/matmul -n 1024 -p 3 -r 0 -t 4
    DSM_NODO=1 ./apps/matmul -n 1024 -p 3 -r 1 -t 4
    DSM_NODO=2 ./apps/matmul -n 1024 -p 3 -r 2 -t 4

### Aplicación 2: video

Se convierte el video a RGB crudo y se procesa repartido entre los nodos. Cada
cuadro pasa a escala de grises y por el detector de bordes de Sobel, y además se
buscan los cambios de escena.

    scripts/preparar_video.sh mi_video.mp4 640        # imprime el comando exacto
    DSM_NODO=0 ./apps/video -i mi_video.rgb -w 640 -h 360 -p 3 -r 0 -t 2 -o bordes.gray
    DSM_NODO=1 ./apps/video -p 3 -r 1 -t 2
    DSM_NODO=2 ./apps/video -p 3 -r 2 -t 2
    ffmpeg -f rawvideo -pix_fmt gray -s 640x360 -r 25 -i bordes.gray bordes.mp4

Un video de 640×360 ocupa unos 0,66 MiB por cuadro de entrada más 0,22 MiB de
salida. Ajuste `memoria_por_nodo` según la duración del video.

### Pruebas

    DSM_NODO=k ./apps/coherencia -p 3 -r k -k 10000     # escrituras intercaladas (MSI)
    DSM_NODO=k ./apps/contador -p 3 -r k -t 2 -k 300    # candados; con -s, sin candado

Para correr una aplicación en P nodos de esta misma máquina:

    scripts/prueba_local.sh matmul 3 -n 512 -t 4
    scripts/prueba_local.sh contador 3 -t 2 -k 300

## Consultas

    ./dsmctl -n 0 nodos              # nodos, estado y memoria del sistema
    ./dsmctl -n 0 paginas            # tabla de páginas de este nodo
    ./dsmctl -n 0 -g paginas         # tabla de páginas de todos los nodos
    ./dsmctl -n 0 -g segmentos       # segmentos, hogares en orden y procesos que los usan
    ./dsmctl -n 0 -g estadisticas    # fallos, transferencias, desalojos, latencia

Monitor web de cada nodo, que se actualiza cada 2 s:

- `http://localhost:27501/` muestra el nodo 0, y `http://localhost:27501/global` todo el sistema.
- El nodo k está en el puerto `27500 + 2k + 1`.

## API

    int   dsm_iniciar(const char *conf);
    void  dsm_terminar(void);
    void *dsm_malloc(size_t tam, const char *nombre, dsm_politica_t pol);
    void *dsm_buscar(const char *nombre, size_t *tam);
    int   dsm_free(void *ptr);
    int   dsm_barrera(const char *nombre, int n);
    int   dsm_candado(const char *nombre);
    int   dsm_soltar(const char *nombre);
    int   dsm_info(dsm_info_t *info);
    int   dsm_ultimo_error(void);
    const char *dsm_strerror(int codigo);

No use `read`, `fread` ni `recv` para escribir directamente en memoria del DSM.
Esas funciones escriben desde el núcleo, y una página sin permiso ahí produce el
error EFAULT en lugar de un fallo de página. Lea a un buffer local y copie con
`memcpy`, como hace `apps/video.c`.

## Organización

    include/dsm.h, include/protocolo.h   API y mensajes
    src/dsmd/      demonio: red, membresía, tabla de páginas y desalojo (dsm.c),
                   MSI (directorio.c), segmentos, barreras y candados, monitor
    src/libdsm/    biblioteca de las aplicaciones: API y manejador de SIGSEGV
    src/comun/     lectura de dsm.conf y textos de error
    tools/dsmctl.c consultas
    apps/          matmul, video, coherencia y contador
    scripts/       prueba_local.sh y preparar_video.sh

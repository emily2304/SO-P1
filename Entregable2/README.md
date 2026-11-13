# DSM: memoria compartida distribuida (prototipo 1)

IC-6600 Principios de Sistemas Operativos, II Semestre 2026.
Rodrigo Sebastián Donoso Chaves y Emily Sánchez.

Cada nodo ejecuta un demonio `dsmd`. Las aplicaciones se enlazan con `libdsm.a`,
reservan memoria con `dsm_malloc` y la usan con punteros normales; los fallos de
página (SIGSEGV) se resuelven pidiendo la página por la red con el protocolo MSI.

## Compilar

Requiere Linux x86-64 (kernel 4.17 o más nuevo), gcc y make.

    make            # dsmd, dsmctl, libdsm.a y las apps de prueba
    make clean

## Configurar

Todos los nodos usan el mismo archivo `dsm.conf` (hay uno de ejemplo en `conf/`).
Si un nodo tiene un archivo distinto, su ingreso se rechaza.

- `puerto_minimo` / `puerto_maximo`: rango de la pareja (27500 a 27599). El nodo k
  escucha en `puerto_minimo + 2k`.
- `semilla`: IP y puerto del nodo fundador. En una sola máquina, `127.0.0.1:27500`;
  en el laboratorio, la IP de la máquina donde se inicia el nodo 0.
- `memoria_por_nodo`: memoria que aporta cada nodo como hogar.

## Ejecutar

En la máquina del nodo fundador:

    cp conf/dsm.conf .
    ./dsmd -f

En cada máquina (o terminal) adicional, con el mismo `dsm.conf`:

    ./dsmd

Cada demonio imprime su número de nodo. Las aplicaciones se conectan al demonio
indicado en la variable `DSM_NODO` (por defecto 0):

    DSM_NODO=0 ./apps/matmul -n 512 -p 3 -r 0 -t 4    # proceso 0: crea las matrices
    DSM_NODO=1 ./apps/matmul -n 512 -p 3 -r 1 -t 4
    DSM_NODO=2 ./apps/matmul -n 512 -p 3 -r 2 -t 4

`-p` es la cantidad de procesos, `-r` el número de este proceso, `-t` los hilos
por proceso y `-e` una etiqueta para los nombres de los segmentos (los segmentos
no se liberan en este prototipo, así que una segunda corrida necesita otra
etiqueta o reiniciar los demonios).

Prueba de coherencia (escrituras intercaladas sobre las mismas páginas):

    DSM_NODO=k ./apps/coherencia -p 3 -r k -k 10000

Para probar todo en una sola máquina:

    scripts/prueba_local.sh matmul 3 -n 512 -t 4
    scripts/prueba_local.sh coherencia 4 -k 20000

## Consultas

    ./dsmctl -n 0 nodos          # tabla de nodos
    ./dsmctl -n 0 paginas        # tabla de páginas del nodo
    ./dsmctl -n 0 segmentos      # segmentos y hogar de cada tramo
    ./dsmctl -n 0 estadisticas   # fallos, páginas transferidas, latencia

## Organización

    include/dsm.h, include/protocolo.h   API y mensajes
    src/dsmd/      demonio: red, membresía, tabla de páginas, MSI, segmentos, barreras
    src/libdsm/    biblioteca de las aplicaciones: API y manejador de SIGSEGV
    src/comun/     lectura de dsm.conf y textos de error
    tools/dsmctl.c consultas
    apps/          matmul y coherencia

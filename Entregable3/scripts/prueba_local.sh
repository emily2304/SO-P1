#!/bin/bash
# Levanta P nodos en esta maquina y corre una aplicacion de prueba en cada uno.
# uso: scripts/prueba_local.sh matmul|coherencia|video|contador P [argumentos de la aplicacion]
# ejemplos: scripts/prueba_local.sh matmul 3 -n 512 -t 4
#           scripts/prueba_local.sh contador 3 -t 2 -k 300
#           scripts/prueba_local.sh video 3 -i video.rgb -w 640 -h 360 -t 2
cd "$(dirname "$0")/.."
APP=${1:-matmul}; P=${2:-3}; shift 2
[ -f dsm.conf ] || cp conf/dsm.conf .
pkill -u "$USER" -x dsmd 2>/dev/null; sleep 0.3
rm -f /tmp/dsm-27500-*.sock

./dsmd -f 2> /tmp/dsmd-0.log &
sleep 0.5
for k in $(seq 1 $((P - 1))); do ./dsmd 2> /tmp/dsmd-$k.log & sleep 0.5; done
for k in $(seq 0 $((P - 1))); do          # esperar a que cada demonio abra su socket
    for i in $(seq 50); do [ -S /tmp/dsm-27500-$k.sock ] && break; sleep 0.1; done
done

DSM_NODO=0 timeout 60 ./apps/$APP -p $P -r 0 "$@" > /tmp/app-0.txt 2>&1 &
APPS=$!
sleep 0.3
for k in $(seq 1 $((P - 1))); do
    DSM_NODO=$k timeout 60 ./apps/$APP -p $P -r $k "$@" > /tmp/app-$k.txt 2>&1 &
    APPS="$APPS $!"
done
wait $APPS
for k in $(seq 0 $((P - 1))); do echo "--- nodo $k"; cat /tmp/app-$k.txt; done
for k in $(seq 0 $((P - 1))); do echo "=== dsmctl -n $k estadisticas"; ./dsmctl -n $k estadisticas; done
pkill -u "$USER" -x dsmd

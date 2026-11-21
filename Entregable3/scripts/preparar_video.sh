#!/bin/bash
# Convierte cualquier video (mp4, mkv, ...) al formato crudo RGB24 que carga apps/video.
# uso: scripts/preparar_video.sh entrada.mp4 [ancho] [segundos]
#   ancho: ancho de salida en pixeles (por defecto 640; el alto conserva la proporcion)
#   segundos: tomar solo los primeros N segundos (por defecto todo el video)
# Imprime el comando para correr la aplicacion con las dimensiones correctas.
set -e
ENTRADA=$1; ANCHO=${2:-640}; SEG=$3
[ -f "$ENTRADA" ] || { echo "uso: $0 entrada.mp4 [ancho] [segundos]"; exit 2; }
SALIDA="${ENTRADA%.*}.rgb"
read W H < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
             -of csv=p=0:s=x "$ENTRADA" | tr x ' ')
ALTO=$(( (ANCHO * H / W + 1) / 2 * 2 ))          # misma proporcion, alto par
LIMITE=""; [ -n "$SEG" ] && LIMITE="-t $SEG"
ffmpeg -v error -y $LIMITE -i "$ENTRADA" -vf "scale=$ANCHO:$ALTO" -pix_fmt rgb24 -f rawvideo "$SALIDA"
BYTES=$(stat -c %s "$SALIDA")
CUADROS=$((BYTES / (ANCHO * ALTO * 3)))
echo "listo: $SALIDA ($CUADROS cuadros de ${ANCHO}x${ALTO}, $((BYTES / 1048576)) MiB)"
echo "ejemplo con 3 procesos (uno por nodo):"
echo "  DSM_NODO=0 ./apps/video -i $SALIDA -w $ANCHO -h $ALTO -p 3 -r 0 -t 2 -o bordes.gray"
echo "  DSM_NODO=1 ./apps/video -p 3 -r 1 -t 2"
echo "  DSM_NODO=2 ./apps/video -p 3 -r 2 -t 2"

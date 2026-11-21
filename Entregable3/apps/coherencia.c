/* coherencia - prueba del protocolo MSI con escrituras intercaladas.
 * Cada proceso incrementa solo los elementos i con i % procesos == rango, pero
 * todos comparten las mismas paginas, asi que la pagina pasa de un nodo a otro
 * mientras escriben. Si el protocolo perdiera una escritura, algun elemento
 * terminaria con menos de 'rondas'. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "dsm.h"

int main(int argc, char **argv)
{
    int procs = 2, rango = 0, rondas = 50, elems = 4096, op;
    while ((op = getopt(argc, argv, "p:r:k:e:")) != -1) {
        if (op == 'p') procs = atoi(optarg);
        else if (op == 'r') rango = atoi(optarg);
        else if (op == 'k') rondas = atoi(optarg);
        else if (op == 'e') elems = atoi(optarg);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);      /* una linea a la vez, aunque se corte el proceso */
    if (dsm_iniciar(NULL) != DSM_OK) { fprintf(stderr, "coherencia: no hay dsmd\n"); return 1; }

    volatile int *v = rango == 0 ? dsm_malloc(elems * sizeof(int), "coherencia", DSM_REPARTIDA) : NULL;
    dsm_barrera("coh_inicio", procs);
    if (!v) v = dsm_buscar("coherencia", NULL);
    if (!v) { fprintf(stderr, "coherencia: %s\n", dsm_strerror(dsm_ultimo_error())); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; k < rondas; k++)
        for (int i = rango; i < elems; i += procs) v[i]++;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("proceso %d: %d rondas en %.3f s\n", rango, rondas,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    dsm_barrera("coh_fin", procs);

    if (rango == 0) {
        int malos = 0;
        for (int i = 0; i < elems; i++) malos += v[i] != rondas;
        printf("verificacion: %s (%d elementos distintos de %d)\n", malos ? "INCORRECTA" : "correcta", malos, rondas);
        dsm_free((void *)v);
    }
    dsm_terminar();
    return 0;
}

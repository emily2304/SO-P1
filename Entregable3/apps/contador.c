/* contador - prueba de los candados distribuidos.
 *
 * Todos los hilos de todos los procesos incrementan el MISMO contador. Un
 * incremento (leer, sumar, escribir) no es atomico entre nodos: entre la
 * lectura y la escritura otro nodo puede tomar la pagina y escribir, y ese
 * incremento se pierde. Con dsm_candado alrededor de cada incremento, el valor
 * final debe ser exactamente procesos x hilos x incrementos. Con -s (sin
 * candado) se puede ver cuantos se pierden.
 *
 * uso: contador -p procesos -r rango [-t hilos] [-k incrementos] [-s]
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "dsm.h"

static volatile long *contador;
static int incrementos = 200, sin_candado = 0;

static void *trabajador(void *arg)
{
    (void)arg;
    for (int i = 0; i < incrementos; i++) {
        if (!sin_candado) dsm_candado("contador");
        long v = *contador;             /* leer ... */
        *contador = v + 1;              /* ... y escribir: sin candado, otro nodo puede colarse en medio */
        if (!sin_candado) dsm_soltar("contador");
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int procs = 1, rango = 0, hilos = 2, op;
    while ((op = getopt(argc, argv, "p:r:t:k:s")) != -1) {
        if (op == 'p') procs = atoi(optarg);
        else if (op == 'r') rango = atoi(optarg);
        else if (op == 't') hilos = atoi(optarg);
        else if (op == 'k') incrementos = atoi(optarg);
        else if (op == 's') sin_candado = 1;
    }
    if (hilos < 1 || hilos > 64 || procs < 1 || rango < 0 || rango >= procs) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);      /* una linea a la vez, aunque se corte el proceso */
    if (dsm_iniciar(NULL) != DSM_OK) { fprintf(stderr, "contador: no hay dsmd\n"); return 1; }

    if (rango == 0) contador = dsm_malloc(sizeof(long), "contador", DSM_LOCAL_PRIMERO);
    dsm_barrera("cont_inicio", procs);
    if (!contador) contador = dsm_buscar("contador", NULL);
    if (!contador) { fprintf(stderr, "contador: %s\n", dsm_strerror(dsm_ultimo_error())); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th[64];
    for (int h = 0; h < hilos; h++) pthread_create(&th[h], NULL, trabajador, NULL);
    for (int h = 0; h < hilos; h++) pthread_join(th[h], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("proceso %d: %d hilos x %d incrementos %s en %.2f s\n", rango, hilos, incrementos,
           sin_candado ? "SIN candado" : "con candado", s);
    dsm_barrera("cont_fin", procs);

    if (rango == 0) {
        long esperado = (long)procs * hilos * incrementos;
        printf("contador = %ld, esperado %ld -> %s (%ld incrementos perdidos)\n", *contador, esperado,
               *contador == esperado ? "correcto" : "INCORRECTO", esperado - *contador);
        dsm_free((void *)contador);
    }
    dsm_terminar();
    return 0;
}

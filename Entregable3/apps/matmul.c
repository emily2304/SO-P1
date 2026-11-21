/* matmul - multiplicacion de matrices C = A x B sobre el DSM.
 * El proceso 0 crea y llena A y B; cada proceso calcula un bloque de filas de C
 * con varios hilos; al final el proceso 0 verifica el resultado completo. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dsm.h"

static int n, hilos = 2;
static double *A, *B, *C;

static double valor_a(int i, int j) { return (i * 7 + j * 3) % 11 - 5; }
static double valor_b(int i, int j) { return (i * 5 + j * 2) % 13 - 6; }

static double ahora(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct { int ini, fin; } rango_t;

static void *trabajador(void *arg)
{
    rango_t *r = arg;
    for (int i = r->ini; i < r->fin; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += A[(size_t)i * n + k] * B[(size_t)k * n + j];
            C[(size_t)i * n + j] = s;
        }
    return NULL;
}

static void *segmento(const char *etq, const char *m, int crear)
{
    char nombre[DSM_MAX_NOMBRE];
    snprintf(nombre, sizeof nombre, "%s_%s", etq, m);
    void *p = crear ? dsm_malloc((size_t)n * n * sizeof(double), nombre, DSM_REPARTIDA)
                    : dsm_buscar(nombre, NULL);
    if (!p) {
        fprintf(stderr, "matmul: segmento %s: %s\n", nombre, dsm_strerror(dsm_ultimo_error()));
        exit(1);
    }
    return p;
}

int main(int argc, char **argv)
{
    int procs = 1, rango = 0, op;
    const char *etq = "mm";
    n = 256;
    while ((op = getopt(argc, argv, "n:p:r:t:e:")) != -1) {
        switch (op) {
        case 'n': n = atoi(optarg); break;
        case 'p': procs = atoi(optarg); break;
        case 'r': rango = atoi(optarg); break;
        case 't': hilos = atoi(optarg); break;
        case 'e': etq = optarg; break;
        default:
            fprintf(stderr, "uso: %s -n N -p procesos -r rango [-t hilos] [-e etiqueta]\n", argv[0]);
            return 2;
        }
    }
    if (n < 1 || procs < 1 || rango < 0 || rango >= procs || hilos < 1) return 2;

    setvbuf(stdout, NULL, _IOLBF, 0);      /* una linea a la vez, aunque se corte el proceso */
    int r = dsm_iniciar(NULL);
    if (r != DSM_OK) { fprintf(stderr, "matmul: %s\n", dsm_strerror(r)); return 1; }
    dsm_info_t inf;
    dsm_info(&inf);
    printf("nodo %u, %u nodos en linea, %llu MiB libres en el cluster\n", inf.id_nodo,
           inf.nodos_en_linea, (unsigned long long)(inf.memoria_libre >> 20));

    char bi[48], bf[48];
    snprintf(bi, sizeof bi, "%s_inicio", etq);
    snprintf(bf, sizeof bf, "%s_fin", etq);
    double t0 = ahora();
    if (rango == 0) {
        A = segmento(etq, "a", 1);
        B = segmento(etq, "b", 1);
        C = segmento(etq, "c", 1);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                A[(size_t)i * n + j] = valor_a(i, j);
                B[(size_t)i * n + j] = valor_b(i, j);
            }
        printf("matrices de %dx%d creadas y llenadas en %.3f s (%.1f MiB en el DSM)\n",
               n, n, ahora() - t0, 3.0 * n * n * sizeof(double) / (1 << 20));
    }
    dsm_barrera(bi, procs);
    if (rango != 0) {
        A = segmento(etq, "a", 0);
        B = segmento(etq, "b", 0);
        C = segmento(etq, "c", 0);
    }

    int ini = (long)n * rango / procs, fin = (long)n * (rango + 1) / procs;
    double t1 = ahora();
    pthread_t th[64];
    rango_t rg[64];
    if (hilos > 64) hilos = 64;
    for (int h = 0; h < hilos; h++) {
        rg[h].ini = ini + (fin - ini) * h / hilos;
        rg[h].fin = ini + (fin - ini) * (h + 1) / hilos;
        pthread_create(&th[h], NULL, trabajador, &rg[h]);
    }
    for (int h = 0; h < hilos; h++) pthread_join(th[h], NULL);
    printf("proceso %d: filas %d a %d con %d hilos en %.3f s\n", rango, ini, fin - 1, hilos, ahora() - t1);
    dsm_barrera(bf, procs);

    if (rango == 0) {
        long errores = 0;
        double *fila = malloc(n * sizeof(double));
        for (int i = 0; i < n; i++) {
            memset(fila, 0, n * sizeof(double));
            for (int k = 0; k < n; k++) {
                double a = valor_a(i, k);
                for (int j = 0; j < n; j++) fila[j] += a * valor_b(k, j);
            }
            for (int j = 0; j < n; j++)
                if (C[(size_t)i * n + j] != fila[j]) errores++;
        }
        free(fila);
        printf("verificacion: %s (%ld diferencias); tiempo total %.3f s\n",
               errores ? "INCORRECTA" : "correcta", errores, ahora() - t0);
        /* prototipo 2: los segmentos se liberan, asi otra corrida puede usar los mismos nombres */
        dsm_free(A);
        dsm_free(B);
        dsm_free(C);
    }
    dsm_terminar();
    return 0;
}

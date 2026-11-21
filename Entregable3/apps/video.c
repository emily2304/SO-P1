/* video - aplicacion 2: procesamiento de un video grande guardado en el DSM.
 *
 * El proceso 0 carga en memoria compartida un video completo en formato crudo
 * RGB24 (lo produce scripts/preparar_video.sh con ffmpeg a partir de cualquier
 * .mp4). Despues cada proceso toma un bloque de cuadros y, con varios hilos,
 * calcula para cada cuadro:
 *   - la conversion a escala de grises (luma de la recomendacion ITU-R BT.601),
 *   - el detector de bordes de Sobel sobre esa imagen (magnitud |Gx| + |Gy|),
 *   - el brillo medio y la proporcion de pixeles de borde.
 * Los cuadros de bordes quedan en otro segmento del DSM. Al final el proceso 0
 * escribe el video de bordes a un archivo, lista los cambios de escena (saltos
 * grandes de brillo entre cuadros seguidos) y verifica cada cuadro contra el
 * mismo calculo hecho con el archivo original en memoria local, fuera del DSM.
 *
 * uso: video -i entrada.rgb -w ancho -h alto -p procesos -r rango [-t hilos]
 *            [-o bordes.gray] [-u umbral] [-e etiqueta]
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "dsm.h"

typedef struct {
    volatile int32_t ancho, alto, cuadros, listo;
    double brillo[];            /* cuadros valores; despues, cuadros valores de bordes */
} meta_t;

static int ancho, alto, cuadros, hilos = 2;
static uint8_t *entrada, *salida;
static meta_t *meta;
static int primero, ultimo;     /* cuadros que procesa este proceso: [primero, ultimo) */

static double ahora(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Gris + Sobel de un cuadro. 'gris' es un buffer local de ancho*alto. */
static void procesar_cuadro(const uint8_t *rgb, uint8_t *bordes, uint8_t *gris,
                            double *brillo, double *prop_bordes)
{
    size_t n = (size_t)ancho * alto;
    uint64_t suma = 0;
    for (size_t i = 0; i < n; i++) {
        /* Y = 0.299 R + 0.587 G + 0.114 B, en enteros: (77 R + 150 G + 29 B) / 256 */
        gris[i] = (77 * rgb[3 * i] + 150 * rgb[3 * i + 1] + 29 * rgb[3 * i + 2]) >> 8;
        suma += gris[i];
    }
    size_t fuertes = 0;
    for (int y = 0; y < alto; y++)
        for (int x = 0; x < ancho; x++) {
            int v = 0;
            if (y > 0 && y < alto - 1 && x > 0 && x < ancho - 1) {
                const uint8_t *p = gris + (size_t)y * ancho + x;
                int gx = (p[-ancho + 1] + 2 * p[1] + p[ancho + 1]) - (p[-ancho - 1] + 2 * p[-1] + p[ancho - 1]);
                int gy = (p[ancho - 1] + 2 * p[ancho] + p[ancho + 1]) - (p[-ancho - 1] + 2 * p[-ancho] + p[-ancho + 1]);
                v = abs(gx) + abs(gy);
                if (v > 255) v = 255;
            }
            bordes[(size_t)y * ancho + x] = v;
            fuertes += v > 96;
        }
    *brillo = (double)suma / n;
    *prop_bordes = (double)fuertes / n;
}

typedef struct { int h; } arg_t;

static void *trabajador(void *a)
{
    int h = ((arg_t *)a)->h;
    size_t n = (size_t)ancho * alto;
    uint8_t *gris = malloc(n), *bordes = malloc(n);
    for (int f = primero + h; f < ultimo; f += hilos) {
        double b, pb;
        procesar_cuadro(entrada + (size_t)f * n * 3, bordes, gris, &b, &pb);
        memcpy(salida + (size_t)f * n, bordes, n);      /* una escritura por cuadro */
        meta->brillo[f] = b;
        meta->brillo[cuadros + f] = pb;
    }
    free(gris);
    free(bordes);
    return NULL;
}

static void *segmento(const char *etq, const char *parte, size_t tam, int crear)
{
    char nombre[DSM_MAX_NOMBRE];
    snprintf(nombre, sizeof nombre, "%s_%s", etq, parte);
    void *p = crear ? dsm_malloc(tam, nombre, DSM_REPARTIDA) : dsm_buscar(nombre, NULL);
    if (!p) {
        fprintf(stderr, "video: segmento %s: %s\n", nombre, dsm_strerror(dsm_ultimo_error()));
        exit(1);
    }
    return p;
}

int main(int argc, char **argv)
{
    const char *ruta = NULL, *ruta_salida = NULL, *etq = "vid";
    int procs = 1, rango = 0, op;
    double umbral = 12.0;
    while ((op = getopt(argc, argv, "i:w:h:p:r:t:o:u:e:")) != -1) {
        switch (op) {
        case 'i': ruta = optarg; break;
        case 'w': ancho = atoi(optarg); break;
        case 'h': alto = atoi(optarg); break;
        case 'p': procs = atoi(optarg); break;
        case 'r': rango = atoi(optarg); break;
        case 't': hilos = atoi(optarg); break;
        case 'o': ruta_salida = optarg; break;
        case 'u': umbral = atof(optarg); break;
        case 'e': etq = optarg; break;
        default:
            fprintf(stderr, "uso: %s -i entrada.rgb -w ancho -h alto -p procesos -r rango "
                            "[-t hilos] [-o bordes.gray] [-u umbral] [-e etiqueta]\n", argv[0]);
            return 2;
        }
    }
    if (procs < 1 || rango < 0 || rango >= procs || hilos < 1 || hilos > 64) return 2;
    if (rango == 0 && (!ruta || ancho < 3 || alto < 3)) {
        fprintf(stderr, "video: el proceso 0 necesita -i, -w y -h\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);      /* una linea a la vez, aunque se corte el proceso */
    int r = dsm_iniciar(NULL);
    if (r != DSM_OK) { fprintf(stderr, "video: %s\n", dsm_strerror(r)); return 1; }
    dsm_info_t inf;
    dsm_info(&inf);
    printf("nodo %u, %u nodos en linea, %llu MiB libres en el cluster\n", inf.id_nodo,
           inf.nodos_en_linea, (unsigned long long)(inf.memoria_libre >> 20));

    char b_inicio[48], b_fin[48];
    snprintf(b_inicio, sizeof b_inicio, "%s_inicio", etq);
    snprintf(b_fin, sizeof b_fin, "%s_fin", etq);
    double t0 = ahora();

    if (rango == 0) {
        struct stat st;
        FILE *f = fopen(ruta, "rb");
        if (!f || fstat(fileno(f), &st) < 0) { perror(ruta); return 1; }
        size_t cuadro = (size_t)ancho * alto * 3;
        cuadros = st.st_size / cuadro;
        if (cuadros < 1) { fprintf(stderr, "video: el archivo no tiene ni un cuadro de %dx%d\n", ancho, alto); return 1; }
        size_t tam_ent = (size_t)cuadros * cuadro, tam_sal = (size_t)cuadros * ancho * alto;
        meta = segmento(etq, "meta", sizeof(meta_t) + 2 * cuadros * sizeof(double), 1);
        entrada = segmento(etq, "rgb", tam_ent, 1);
        salida = segmento(etq, "bordes", tam_sal, 1);
        /* Cargar el archivo en la memoria distribuida por bloques. No se puede
         * hacer fread directo al DSM: read() escribe desde el nucleo, y una pagina
         * con PROT_NONE ahi no produce SIGSEGV sino el error EFAULT. Por eso se lee
         * a un buffer local y se copia con memcpy, que si provoca los fallos. */
        size_t hecho = 0, bloque = 4u << 20;
        uint8_t *buf = malloc(bloque);
        while (hecho < tam_ent) {
            size_t k = fread(buf, 1, tam_ent - hecho < bloque ? tam_ent - hecho : bloque, f);
            if (k == 0) break;
            memcpy(entrada + hecho, buf, k);
            hecho += k;
        }
        free(buf);
        fclose(f);
        if (hecho < tam_ent) { fprintf(stderr, "video: solo se leyeron %zu bytes\n", hecho); return 1; }
        meta->ancho = ancho;
        meta->alto = alto;
        meta->cuadros = cuadros;
        double t = ahora() - t0;
        printf("video de %d cuadros de %dx%d cargado en el DSM: %.1f MiB de entrada + %.1f MiB de salida, "
               "en %.2f s (%.1f MiB/s)\n", cuadros, ancho, alto, tam_ent / 1048576.0, tam_sal / 1048576.0,
               t, tam_ent / 1048576.0 / t);
    }
    dsm_barrera(b_inicio, procs);
    if (rango != 0) {
        meta = segmento(etq, "meta", 0, 0);
        ancho = meta->ancho;
        alto = meta->alto;
        cuadros = meta->cuadros;
        entrada = segmento(etq, "rgb", 0, 0);
        salida = segmento(etq, "bordes", 0, 0);
    }

    primero = (long)cuadros * rango / procs;
    ultimo = (long)cuadros * (rango + 1) / procs;
    double t1 = ahora();
    pthread_t th[64];
    arg_t ar[64];
    for (int h = 0; h < hilos; h++) {
        ar[h].h = h;
        pthread_create(&th[h], NULL, trabajador, &ar[h]);
    }
    for (int h = 0; h < hilos; h++) pthread_join(th[h], NULL);
    double tp = ahora() - t1;
    printf("proceso %d: cuadros %d a %d con %d hilos en %.2f s (%.1f cuadros/s)\n", rango, primero,
           ultimo - 1, hilos, tp, (ultimo - primero) / (tp > 0 ? tp : 1e-9));
    dsm_barrera(b_fin, procs);

    if (rango == 0) {
        /* 1. video de bordes al disco, leyendo del DSM */
        size_t n = (size_t)ancho * alto;
        if (ruta_salida) {
            FILE *o = fopen(ruta_salida, "wb");
            if (o) {
                /* tampoco fwrite directo desde el DSM: write() tendria el mismo problema */
                uint8_t *cuadro = malloc(n);
                for (int k = 0; k < cuadros; k++) {
                    memcpy(cuadro, salida + (size_t)k * n, n);
                    fwrite(cuadro, 1, n, o);
                }
                free(cuadro);
                fclose(o);
                printf("video de bordes en %s; para verlo:\n  ffmpeg -f rawvideo -pix_fmt gray -s %dx%d "
                       "-r 25 -i %s bordes.mp4\n", ruta_salida, ancho, alto, ruta_salida);
            }
        }
        /* 2. cambios de escena: saltos de brillo medio entre cuadros seguidos */
        int escenas = 0;
        printf("cambios de escena (salto de brillo > %.0f):", umbral);
        for (int f = 1; f < cuadros; f++)
            if (fabs(meta->brillo[f] - meta->brillo[f - 1]) > umbral) {
                if (escenas < 20) printf(" %d", f);
                escenas++;
            }
        if (escenas == 0) printf(" ninguno\n");
        else if (escenas > 20) printf(" ... (%d en total)\n", escenas);
        else printf(" (%d en total)\n", escenas);
        /* 3. verificacion contra el archivo original, en memoria local */
        FILE *f = fopen(ruta, "rb");
        uint8_t *rgb = malloc(n * 3), *gris = malloc(n), *bordes = malloc(n);
        int malos = 0;
        double t2 = ahora();
        for (int k = 0; k < cuadros && f; k++) {
            if (fread(rgb, 1, n * 3, f) != n * 3) { malos++; continue; }
            double b, pb;
            procesar_cuadro(rgb, bordes, gris, &b, &pb);
            if (memcmp(bordes, salida + (size_t)k * n, n) || b != meta->brillo[k] || pb != meta->brillo[cuadros + k]) {
                size_t px = 0;
                for (size_t i = 0; i < n; i++) px += bordes[i] != salida[(size_t)k * n + i];
                if (malos < 5) printf("  cuadro %d distinto: %zu pixeles, brillo %.3f contra %.3f\n",
                                      k, px, meta->brillo[k], b);
                malos++;
            }
        }
        if (f) fclose(f);
        printf("verificacion: %s (%d de %d cuadros distintos al calculo local, %.2f s); tiempo total %.2f s\n",
               malos ? "INCORRECTA" : "correcta", malos, cuadros, ahora() - t2, ahora() - t0);
        free(rgb); free(gris); free(bordes);
        dsm_free(salida);
        dsm_free(entrada);
        dsm_free(meta);
    }
    dsm_terminar();
    return 0;
}

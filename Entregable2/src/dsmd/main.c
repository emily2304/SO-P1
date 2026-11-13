/* main.c - arranque del demonio dsmd y ciclos de los hilos trabajadores y de ordenes */
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsmd.h"

dsmd_t g;

/* Fallos y peticiones de las apps locales: pueden esperar al hogar de otro nodo */
static void *hilo_locales(void *arg)
{
    (void)arg;
    for (;;) {
        mensaje_t *m = desencolar(&g.locales);
        if (m->loc.tipo == LOC_FALLO) resolver_fallo(m);
        else if (m->loc.tipo == LOC_MALLOC) atender_malloc(m);
        else mensaje_liberar(m);
    }
    return NULL;
}

/* Lado del hogar: solo espera respuestas que atienden los hilos de ordenes de
 * otros nodos, nunca a otro trabajador; por eso va en un grupo aparte */
static void *hilo_hogar(void *arg)
{
    (void)arg;
    for (;;) atender_page_request(desencolar(&g.peticiones));
    return NULL;
}

/* Mensajes que se atienden sin esperar a otro nodo, en el orden de llegada */
static void *hilo_ordenes(void *arg)
{
    (void)arg;
    for (;;) {
        mensaje_t *m = desencolar(&g.ordenes);
        if (m->local) { atender_barrera_local(m); continue; }
        switch (m->cab.tipo) {
        case MSG_JOIN:            atender_join(m); break;
        case MSG_NODE_ANNOUNCE:   atender_announce(m); break;
        case MSG_PAGE_DATA:       atender_page_data(m); break;
        case MSG_INVALIDATE:      atender_invalidate(m); break;
        case MSG_FETCH:           atender_fetch(m); break;
        case MSG_SEGMENT_CREATE:  atender_segment_create(m); break;
        case MSG_BARRIER_ENTER:   atender_barrier_enter(m); break;
        case MSG_BARRIER_RELEASE: atender_barrier_release(m); break;
        case MSG_NODE_DOWN:
            if (m->len >= sizeof(msg_node_down_t)) nodo_caido(ntohs(((msg_node_down_t *)m->cuerpo)->id));
            mensaje_liberar(m);
            break;
        default: mensaje_liberar(m);
        }
    }
    return NULL;
}

static void uso(const char *p)
{
    fprintf(stderr, "uso: %s [-c dsm.conf] [-f]\n"
                    "  -c  archivo de configuracion (por defecto ./dsm.conf)\n"
                    "  -f  fundar el cluster; sin -f el nodo pide ingreso a una semilla\n", p);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *ruta = "dsm.conf";
    int fundar = 0, op;
    while ((op = getopt(argc, argv, "c:fh")) != -1) {
        if (op == 'c') ruta = optarg;
        else if (op == 'f') fundar = 1;
        else uso(argv[0]);
    }
    if (config_leer(ruta, &g.cfg) != DSM_OK) {
        fprintf(stderr, "dsmd: no se pudo leer %s\n", ruta);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);

    pthread_mutex_init(&g.mx_nodos, NULL);
    pthread_mutex_init(&g.mx_segs, NULL);
    pthread_mutex_init(&g.mx_pend, NULL);
    pthread_mutex_init(&g.mx_procs, NULL);
    pthread_cond_init(&g.cv_pend, NULL);
    for (int i = 0; i < MAX_NODOS; i++) {
        g.nodos[i].fd = -1;
        pthread_mutex_init(&g.nodos[i].envio, NULL);
    }
    if (pipe(g.tubo) < 0) return 1;

    int r = fundar ? membresia_fundar() : membresia_unirse();
    if (r != DSM_OK) return 1;
    if (memoria_iniciar() != DSM_OK) {
        LOG("no se pudo reservar el espacio de memoria");
        return 1;
    }
    config_ruta_socket(&g.cfg, g.yo, g.ruta_socket, sizeof g.ruta_socket);
    cola_iniciar(&g.locales, g.cfg.tam_cola);
    cola_iniciar(&g.peticiones, g.cfg.tam_cola);
    cola_iniciar(&g.ordenes, g.cfg.tam_cola);

    pthread_t t;
    pthread_create(&t, NULL, hilo_escuchador, NULL);
    pthread_create(&t, NULL, hilo_latidos, NULL);
    pthread_create(&t, NULL, hilo_ordenes, NULL);
    for (int i = 0; i < g.cfg.hilos_trabajadores; i++) {
        pthread_create(&t, NULL, hilo_locales, NULL);
        pthread_create(&t, NULL, hilo_hogar, NULL);
    }
    LOG("aporta %llu MiB; socket local %s",
        (unsigned long long)(g.cfg.memoria_por_nodo >> 20), g.ruta_socket);
    hilo_clientes(NULL);
    return 0;
}

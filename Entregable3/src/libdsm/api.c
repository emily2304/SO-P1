/* api.c - funciones del API; cada una es una peticion al dsmd local.
 * Prototipo 2: dsm_free, dsm_candado y dsm_soltar. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "config.h"
#include "libdsm.h"

int      dsm_sock = -1;
uint8_t *dsm_base;
uint64_t dsm_tam_espacio;
uint32_t dsm_tam_pagina;

static __thread int ultimo_error;

#define MAX_ESPERAS 64
static struct { int usada, lista; uint32_t id; msg_local_t resp; } esperas[MAX_ESPERAS];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static uint32_t sig_id;

void api_entregar_respuesta(const msg_local_t *m)
{
    pthread_mutex_lock(&mx);
    for (int i = 0; i < MAX_ESPERAS; i++)
        if (esperas[i].usada && esperas[i].id == m->id) {
            esperas[i].resp = *m;
            esperas[i].lista = 1;
        }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mx);
}

/* Envia una peticion al dsmd y espera su respuesta (la recibe el hilo de servicio) */
static int pedir(msg_local_t *m)
{
    if (dsm_sock < 0) return ultimo_error = DSM_ENOINICIADO;
    pthread_mutex_lock(&mx);
    int i = 0;
    while (i < MAX_ESPERAS && esperas[i].usada) i++;
    if (i == MAX_ESPERAS) { pthread_mutex_unlock(&mx); return ultimo_error = DSM_EINVAL; }
    esperas[i].usada = 1;
    esperas[i].lista = 0;
    esperas[i].id = m->id = ++sig_id;
    pthread_mutex_unlock(&mx);

    m->pid = getpid();
    if (send(dsm_sock, m, sizeof *m, MSG_NOSIGNAL) < 0) {
        pthread_mutex_lock(&mx);
        esperas[i].usada = 0;
        pthread_mutex_unlock(&mx);
        return ultimo_error = DSM_ENODEMONIO;
    }
    pthread_mutex_lock(&mx);
    while (!esperas[i].lista) pthread_cond_wait(&cv, &mx);
    *m = esperas[i].resp;
    esperas[i].usada = 0;
    pthread_mutex_unlock(&mx);
    return ultimo_error = m->error;
}

int dsm_iniciar(const char *conf)
{
    if (dsm_sock >= 0) return DSM_OK;
    config_t c;
    if (!conf) conf = getenv("DSM_CONF") ? getenv("DSM_CONF") : "dsm.conf";
    if (config_leer(conf, &c) != DSM_OK) return ultimo_error = DSM_ECONF;
    int nodo = getenv("DSM_NODO") ? atoi(getenv("DSM_NODO")) : 0;

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    config_ruta_socket(&c, nodo, a.sun_path, sizeof a.sun_path);
    int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return ultimo_error = DSM_ENODEMONIO; }

    msg_local_t m = { .tipo = LOC_HOLA, .pid = getpid() };
    send(s, &m, sizeof m, MSG_NOSIGNAL);

    /* la bienvenida trae el memfd del nodo por SCM_RIGHTS */
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct iovec v = { &m, sizeof m };
    struct msghdr mh = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = ctrl, .msg_controllen = sizeof ctrl };
    struct cmsghdr *cm;
    int memfd = -1;
    if (recvmsg(s, &mh, 0) != (ssize_t)sizeof m || m.tipo != LOC_BIENVENIDA ||
        !(cm = CMSG_FIRSTHDR(&mh)) || cm->cmsg_type != SCM_RIGHTS) {
        close(s);
        return ultimo_error = DSM_EPROTOCOLO;
    }
    memcpy(&memfd, CMSG_DATA(cm), sizeof memfd);

    dsm_tam_pagina = m.vpn;
    dsm_tam_espacio = m.valor2;
    void *p = mmap((void *)m.valor, dsm_tam_espacio, PROT_NONE, MAP_SHARED | MAP_FIXED_NOREPLACE, memfd, 0);
    close(memfd);
    if (p != (void *)m.valor) {
        if (p != MAP_FAILED) munmap(p, dsm_tam_espacio);
        close(s);
        return ultimo_error = DSM_ENOMEM;
    }
    dsm_base = p;
    dsm_sock = s;
    if (pager_iniciar() != 0) { dsm_terminar(); return ultimo_error = DSM_EINVAL; }
    return DSM_OK;
}

void dsm_terminar(void)
{
    if (dsm_sock < 0) return;
    pager_terminar();
    close(dsm_sock);
    dsm_sock = -1;
    munmap(dsm_base, dsm_tam_espacio);
}

static void copiar_nombre(char *dst, const char *nombre)
{
    memset(dst, 0, DSM_MAX_NOMBRE);
    if (nombre) strncpy(dst, nombre, DSM_MAX_NOMBRE - 1);
}

void *dsm_malloc(size_t tam, const char *nombre, dsm_politica_t pol)
{
    msg_local_t m = { .tipo = LOC_MALLOC, .valor = tam, .acceso = pol };
    copiar_nombre(m.nombre, nombre);
    if (pedir(&m) != DSM_OK) return NULL;
    return (void *)m.valor;
}

void *dsm_buscar(const char *nombre, size_t *tam)
{
    msg_local_t m = { .tipo = LOC_BUSCAR };
    copiar_nombre(m.nombre, nombre);
    if (pedir(&m) != DSM_OK) return NULL;
    if (tam) *tam = m.valor2;
    return (void *)m.valor;
}

/* Libera un segmento en todo el sistema. ptr debe ser el inicio del segmento. */
int dsm_free(void *ptr)
{
    msg_local_t m = { .tipo = LOC_FREE, .valor = (uint64_t)ptr };
    return pedir(&m);
}

/* Candado distribuido: bloquea hasta que el coordinador lo conceda a este proceso */
int dsm_candado(const char *nombre)
{
    if (!nombre || !*nombre) return ultimo_error = DSM_EINVAL;
    msg_local_t m = { .tipo = LOC_CANDADO };
    copiar_nombre(m.nombre, nombre);
    return pedir(&m);
}

/* Suelta un candado que este proceso tiene; DSM_ENOENT si no lo tenia */
int dsm_soltar(const char *nombre)
{
    if (!nombre || !*nombre) return ultimo_error = DSM_EINVAL;
    msg_local_t m = { .tipo = LOC_SOLTAR };
    copiar_nombre(m.nombre, nombre);
    return pedir(&m);
}

int dsm_barrera(const char *nombre, int n)
{
    if (n <= 0) return ultimo_error = DSM_EINVAL;
    msg_local_t m = { .tipo = LOC_BARRERA, .valor = n };
    copiar_nombre(m.nombre, nombre);
    return pedir(&m);
}

int dsm_info(dsm_info_t *info)
{
    msg_local_t m = { .tipo = LOC_INFO };
    int r = pedir(&m);
    if (r == DSM_OK && info) {
        info->id_nodo = m.ranura;
        info->nodos_en_linea = m.pid;
        info->tam_pagina = m.vpn;
        info->memoria_total = m.valor;
        info->memoria_libre = m.valor2;
    }
    return r;
}

int dsm_ultimo_error(void) { return ultimo_error; }

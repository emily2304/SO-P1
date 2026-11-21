/* sincro.c - barreras y candados distribuidos.
 *
 * Los dos los administra el coordinador (nodo en linea de menor id). Los
 * mensajes se atienden en el hilo de ordenes y nadie se queda bloqueado: una
 * espera es solo una entrada en una tabla, y la respuesta a la aplicacion sale
 * cuando llega el BARRIER_RELEASE o el LOCK_GRANT. Desde el prototipo 2 las
 * tablas tienen un mutex, porque nodo_caido() las toca desde otros hilos. */
#include <arpa/inet.h>
#include <string.h>
#include "dsmd.h"

typedef struct {
    int      usada;
    char     nombre[DSM_MAX_NOMBRE];
    uint32_t cuenta, participantes, generacion;
} barrera_t;

typedef struct {
    int      usada, fd;
    uint32_t id;
    char     nombre[DSM_MAX_NOMBRE];
} espera_t;

/* Candado en el coordinador: quien lo tiene y la cola FIFO de los que esperan */
#define MAX_COLA_CANDADO 64
typedef struct {
    int      usado;
    char     nombre[DSM_MAX_NOMBRE];
    uint16_t nodo;                          /* NADIE si esta libre */
    uint32_t token;
    struct { uint16_t nodo; uint32_t token; } cola[MAX_COLA_CANDADO];
    int      n_cola;
} candado_t;

/* Espera de un proceso local por un candado, o candado que un proceso de este
 * nodo tiene concedido (para saber que token liberar en dsm_soltar) */
typedef struct {
    int      usada, concedido, fd;
    uint32_t id, token;
    char     nombre[DSM_MAX_NOMBRE];
} espera_candado_t;

static barrera_t        barreras[MAX_BARRERAS];     /* solo se usan en el coordinador */
static espera_t         esperas[4 * MAX_PROCS];     /* procesos locales detenidos en una barrera */
static candado_t        candados[MAX_CANDADOS];     /* solo se usan en el coordinador */
static espera_candado_t locales[4 * MAX_PROCS];
static uint32_t         sig_token;
static pthread_mutex_t  mx = PTHREAD_MUTEX_INITIALIZER;

static int mismo(const char *a, const char *b) { return !strncmp(a, b, DSM_MAX_NOMBRE); }

/* ---------------- barreras ---------------- */

static void liberar_locales(const char *nombre)
{
    for (int i = 0; i < 4 * MAX_PROCS; i++)
        if (esperas[i].usada && mismo(esperas[i].nombre, nombre)) {
            responder_app(esperas[i].fd, esperas[i].id, 0, 0, 0);
            esperas[i].usada = 0;
        }
}

static void entrar(uint16_t origen, const msg_barrera_t *mb)
{
    barrera_t *b = NULL, *libre = NULL;
    for (int i = 0; i < MAX_BARRERAS && !b; i++) {
        if (barreras[i].usada && mismo(barreras[i].nombre, mb->nombre)) b = &barreras[i];
        else if (!barreras[i].usada && !libre) libre = &barreras[i];
    }
    if (!b) {
        if (!libre) { LOG("no quedan barreras libres"); return; }
        b = libre;
        memset(b, 0, sizeof *b);
        b->usada = 1;
        memcpy(b->nombre, mb->nombre, DSM_MAX_NOMBRE);
    }
    b->cuenta++;
    b->participantes |= 1u << origen;
    if (b->cuenta < ntohl(mb->n)) return;

    msg_barrera_t r = *mb;
    r.generacion = htonl(b->generacion);
    for (int i = 0; i < MAX_NODOS; i++) {
        if (!(b->participantes & (1u << i))) continue;
        if (i == g.yo) liberar_locales(b->nombre);
        else red_enviar(i, MSG_BARRIER_RELEASE, 0, 0, &r, sizeof r, NULL);
    }
    b->cuenta = b->participantes = 0;
    b->generacion++;
}

void atender_barrera_local(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    int i = 0;
    while (i < 4 * MAX_PROCS && esperas[i].usada) i++;
    if (i == 4 * MAX_PROCS || m->loc.valor == 0) {
        pthread_mutex_unlock(&mx);
        responder_app(m->fd, m->loc.id, DSM_EINVAL, 0, 0);
        mensaje_liberar(m);
        return;
    }
    esperas[i] = (espera_t){ .usada = 1, .fd = m->fd, .id = m->loc.id };
    memcpy(esperas[i].nombre, m->loc.nombre, DSM_MAX_NOMBRE);

    msg_barrera_t mb = { .n = htonl((uint32_t)m->loc.valor) };
    memcpy(mb.nombre, m->loc.nombre, DSM_MAX_NOMBRE);
    uint16_t c = coordinador();
    if (c == g.yo) entrar(g.yo, &mb);
    else if (red_enviar(c, MSG_BARRIER_ENTER, 0, 0, &mb, sizeof mb, NULL)) {
        responder_app(m->fd, m->loc.id, DSM_ENODOCAIDO, 0, 0);
        esperas[i].usada = 0;
    }
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

void atender_barrier_enter(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    if (m->len >= sizeof(msg_barrera_t)) entrar(m->cab.origen, (msg_barrera_t *)m->cuerpo);
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

void atender_barrier_release(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    if (m->len >= sizeof(msg_barrera_t)) liberar_locales(((msg_barrera_t *)m->cuerpo)->nombre);
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

/* ---------------- candados (prototipo 2) ---------------- */

/* Avisa al nodo que pidio el candado que ya es suyo. Con mx tomado. */
static void conceder_candado(const char *nombre, uint16_t nodo, uint32_t token);

static candado_t *buscar_candado(const char *nombre, int crear)
{
    candado_t *libre = NULL;
    for (int i = 0; i < MAX_CANDADOS; i++) {
        if (candados[i].usado && mismo(candados[i].nombre, nombre)) return &candados[i];
        if (!candados[i].usado && !libre) libre = &candados[i];
    }
    if (!crear || !libre) return NULL;
    memset(libre, 0, sizeof *libre);
    libre->usado = 1;
    libre->nodo = NADIE;
    memcpy(libre->nombre, nombre, DSM_MAX_NOMBRE);
    return libre;
}

/* En el coordinador: si esta libre se concede; si no, a la cola */
static void pedir(const char *nombre, uint16_t nodo, uint32_t token)
{
    candado_t *c = buscar_candado(nombre, 1);
    if (!c) { LOG("no quedan candados libres"); return; }
    if (c->nodo == NADIE) {
        c->nodo = nodo;
        c->token = token;
        conceder_candado(c->nombre, nodo, token);
    } else if (c->n_cola < MAX_COLA_CANDADO) {
        c->cola[c->n_cola].nodo = nodo;
        c->cola[c->n_cola].token = token;
        c->n_cola++;
    } else LOG("cola del candado '%s' llena", c->nombre);
}

/* En el coordinador: el que lo tenia lo suelta y pasa al primero de la cola */
static void soltar(const char *nombre, uint16_t nodo, uint32_t token)
{
    candado_t *c = buscar_candado(nombre, 0);
    if (!c || c->nodo != nodo || c->token != token) return;
    if (c->n_cola == 0) { c->nodo = NADIE; return; }
    c->nodo = c->cola[0].nodo;
    c->token = c->cola[0].token;
    memmove(c->cola, c->cola + 1, --c->n_cola * sizeof c->cola[0]);
    conceder_candado(c->nombre, c->nodo, c->token);
}

/* En el nodo que pidio: despierta al proceso que esperaba ese token */
static void concedido(uint32_t token)
{
    for (int i = 0; i < 4 * MAX_PROCS; i++)
        if (locales[i].usada && !locales[i].concedido && locales[i].token == token) {
            locales[i].concedido = 1;
            responder_app(locales[i].fd, locales[i].id, 0, 0, 0);
            return;
        }
}

static void conceder_candado(const char *nombre, uint16_t nodo, uint32_t token)
{
    if (nodo == g.yo) { concedido(token); return; }
    msg_candado_t mc = { .token = htonl(token) };
    memcpy(mc.nombre, nombre, DSM_MAX_NOMBRE);
    red_enviar(nodo, MSG_LOCK_GRANT, 0, 0, &mc, sizeof mc, NULL);
}

void atender_candado_local(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    int i = 0;
    while (i < 4 * MAX_PROCS && locales[i].usada) i++;
    if (i == 4 * MAX_PROCS || !m->loc.nombre[0]) {
        pthread_mutex_unlock(&mx);
        responder_app(m->fd, m->loc.id, DSM_EINVAL, 0, 0);
        mensaje_liberar(m);
        return;
    }
    uint32_t token = ++sig_token;
    locales[i] = (espera_candado_t){ .usada = 1, .fd = m->fd, .id = m->loc.id, .token = token };
    memcpy(locales[i].nombre, m->loc.nombre, DSM_MAX_NOMBRE);
    locales[i].nombre[DSM_MAX_NOMBRE - 1] = 0;

    uint16_t c = coordinador();
    if (c == g.yo) pedir(locales[i].nombre, g.yo, token);
    else {
        msg_candado_t mc = { .token = htonl(token) };
        memcpy(mc.nombre, locales[i].nombre, DSM_MAX_NOMBRE);
        if (red_enviar(c, MSG_LOCK_REQUEST, 0, 0, &mc, sizeof mc, NULL)) {
            responder_app(m->fd, m->loc.id, DSM_ENODOCAIDO, 0, 0);
            locales[i].usada = 0;
        }
    }
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

/* dsm_soltar: se responde de inmediato; el coordinador pasa el candado al siguiente */
void atender_soltar_local(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    int i = 0, err = DSM_ENOENT;
    for (; i < 4 * MAX_PROCS; i++)
        if (locales[i].usada && locales[i].concedido && locales[i].fd == m->fd &&
            mismo(locales[i].nombre, m->loc.nombre)) break;
    if (i < 4 * MAX_PROCS) {
        uint32_t token = locales[i].token;
        locales[i].usada = 0;
        err = DSM_OK;
        uint16_t c = coordinador();
        if (c == g.yo) soltar(m->loc.nombre, g.yo, token);
        else {
            msg_candado_t mc = { .token = htonl(token) };
            memcpy(mc.nombre, m->loc.nombre, DSM_MAX_NOMBRE);
            red_enviar(c, MSG_LOCK_RELEASE, 0, 0, &mc, sizeof mc, NULL);
        }
    }
    pthread_mutex_unlock(&mx);
    responder_app(m->fd, m->loc.id, err, 0, 0);
    mensaje_liberar(m);
}

void atender_lock_request(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    if (m->len >= sizeof(msg_candado_t)) {
        msg_candado_t *mc = (msg_candado_t *)m->cuerpo;
        mc->nombre[DSM_MAX_NOMBRE - 1] = 0;
        pedir(mc->nombre, m->cab.origen, ntohl(mc->token));
    }
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

void atender_lock_grant(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    if (m->len >= sizeof(msg_candado_t)) concedido(ntohl(((msg_candado_t *)m->cuerpo)->token));
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

void atender_lock_release(mensaje_t *m)
{
    pthread_mutex_lock(&mx);
    if (m->len >= sizeof(msg_candado_t)) {
        msg_candado_t *mc = (msg_candado_t *)m->cuerpo;
        mc->nombre[DSM_MAX_NOMBRE - 1] = 0;
        soltar(mc->nombre, m->cab.origen, ntohl(mc->token));
    }
    pthread_mutex_unlock(&mx);
    mensaje_liberar(m);
}

/* Un nodo se cayo: sus candados pasan al siguiente y sus esperas se borran,
 * para que el resto del sistema no se quede bloqueado esperandolo. */
void sincro_nodo_caido(uint16_t id)
{
    pthread_mutex_lock(&mx);
    for (int i = 0; i < MAX_CANDADOS; i++) {
        candado_t *c = &candados[i];
        if (!c->usado) continue;
        int k = 0;
        for (int j = 0; j < c->n_cola; j++)
            if (c->cola[j].nodo != id) c->cola[k++] = c->cola[j];
        c->n_cola = k;
        if (c->nodo == id) {
            LOG("el candado '%s' lo tenia el nodo %u, que se cayo: pasa al siguiente", c->nombre, id);
            soltar(c->nombre, id, c->token);
        }
    }
    pthread_mutex_unlock(&mx);
}

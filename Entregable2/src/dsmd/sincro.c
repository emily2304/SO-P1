/* sincro.c - barreras distribuidas. Todo se atiende en el hilo de ordenes,
 * asi que estas tablas no necesitan candado propio. */
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

static barrera_t barreras[MAX_BARRERAS];    /* solo se usan en el coordinador */
static espera_t  esperas[4 * MAX_PROCS];    /* procesos locales detenidos en una barrera */

static void liberar_locales(const char *nombre)
{
    for (int i = 0; i < 4 * MAX_PROCS; i++)
        if (esperas[i].usada && !strncmp(esperas[i].nombre, nombre, DSM_MAX_NOMBRE)) {
            responder_app(esperas[i].fd, esperas[i].id, 0, 0, 0);
            esperas[i].usada = 0;
        }
}

static void entrar(uint16_t origen, const msg_barrera_t *mb)
{
    barrera_t *b = NULL, *libre = NULL;
    for (int i = 0; i < MAX_BARRERAS && !b; i++) {
        if (barreras[i].usada && !strncmp(barreras[i].nombre, mb->nombre, DSM_MAX_NOMBRE)) b = &barreras[i];
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
    int i = 0;
    while (i < 4 * MAX_PROCS && esperas[i].usada) i++;
    if (i == 4 * MAX_PROCS || m->loc.valor == 0) {
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
    mensaje_liberar(m);
}

void atender_barrier_enter(mensaje_t *m)
{
    if (m->len >= sizeof(msg_barrera_t)) entrar(m->cab.origen, (msg_barrera_t *)m->cuerpo);
    mensaje_liberar(m);
}

void atender_barrier_release(mensaje_t *m)
{
    if (m->len >= sizeof(msg_barrera_t)) liberar_locales(((msg_barrera_t *)m->cuerpo)->nombre);
    mensaje_liberar(m);
}

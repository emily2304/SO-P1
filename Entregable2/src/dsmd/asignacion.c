/* asignacion.c - creacion de segmentos (dsm_malloc) y reparto de hogares */
#include <arpa/inet.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include "dsmd.h"

#define MAX_TRAMOS 16384

/* Da de alta un segmento en este nodo y descuenta la capacidad de hogar propia */
static int registrar_segmento(const msg_segment_create_t *sc, const tramo_t *tr)
{
    if (segmento_por_nombre(sc->nombre)) return DSM_OK;     /* ya se conocia */
    int n = ntohs(sc->cantidad_tramos);
    uint32_t propias = 0;
    for (int i = 0; i < n; i++)
        if (ntohs(tr[i].hogar) == g.yo) propias += ntohl(tr[i].cantidad);

    pthread_mutex_lock(&g.mx_nodos);
    int hay = g.nodos[g.yo].libres >= propias;
    if (hay) g.nodos[g.yo].libres -= propias;
    pthread_mutex_unlock(&g.mx_nodos);
    if (!hay) return DSM_ENOMEM;

    segmento_t *s = calloc(1, sizeof *s);
    s->id = ntohl(sc->id);
    s->vpn_base = ntohl(sc->vpn_base);
    s->paginas = ntohl(sc->paginas);
    s->tam_bytes = be64toh(sc->tam_bytes);
    s->creador = ntohs(sc->creador);
    s->pid_creador = ntohl(sc->pid_creador);
    s->n_tramos = n;
    memcpy(s->nombre, sc->nombre, DSM_MAX_NOMBRE);
    s->tramos = calloc(n, sizeof(tramo_t));
    for (int i = 0; i < n; i++) {
        s->tramos[i].vpn_inicio = ntohl(tr[i].vpn_inicio);
        s->tramos[i].cantidad = ntohl(tr[i].cantidad);
        s->tramos[i].hogar = ntohs(tr[i].hogar);
    }

    pthread_mutex_lock(&g.mx_segs);
    int ok = g.n_segs < MAX_SEGMENTOS;
    if (ok) g.segs[g.n_segs++] = s;
    pthread_mutex_unlock(&g.mx_segs);
    if (!ok) { free(s->tramos); free(s); return DSM_ENOMEM; }
    return DSM_OK;
}

static uint32_t armar_segmento(const segmento_t *s, uint8_t *buf)
{
    msg_segment_create_t *sc = (msg_segment_create_t *)buf;
    tramo_t *tr = (tramo_t *)(sc + 1);
    memset(sc, 0, sizeof *sc);
    sc->id = htonl(s->id);
    sc->vpn_base = htonl(s->vpn_base);
    sc->paginas = htonl(s->paginas);
    sc->tam_bytes = htobe64(s->tam_bytes);
    sc->creador = htons(s->creador);
    sc->cantidad_tramos = htons(s->n_tramos);
    sc->pid_creador = htonl(s->pid_creador);
    memcpy(sc->nombre, s->nombre, DSM_MAX_NOMBRE);
    for (int i = 0; i < s->n_tramos; i++)
        tr[i] = (tramo_t){ .vpn_inicio = htonl(s->tramos[i].vpn_inicio),
                           .cantidad = htonl(s->tramos[i].cantidad),
                           .hogar = htons(s->tramos[i].hogar) };
    return sizeof *sc + s->n_tramos * sizeof(tramo_t);
}

/* Reparte las paginas entre los nodos en linea segun la politica */
static int elegir_hogares(uint32_t vpn_base, uint32_t paginas, int politica, tramo_t *tr, int max)
{
    uint32_t libres[MAX_NODOS] = { 0 };
    pthread_mutex_lock(&g.mx_nodos);
    for (int i = 0; i < MAX_NODOS; i++)
        if (g.nodos[i].en_linea) libres[i] = g.nodos[i].libres;
    pthread_mutex_unlock(&g.mx_nodos);

    int n = 0, nodo = politica == DSM_LOCAL_PRIMERO ? g.yo : 0;
    uint32_t hechas = 0;
    while (hechas < paginas) {
        int vueltas = 0;
        while (libres[nodo] == 0 && vueltas++ < MAX_NODOS) nodo = (nodo + 1) % MAX_NODOS;
        if (libres[nodo] == 0) return DSM_ENOMEM;

        uint32_t bloque = politica == DSM_LOCAL_PRIMERO ? libres[nodo] : g.cfg.bloque_reparto;
        if (bloque > libres[nodo]) bloque = libres[nodo];
        if (bloque > paginas - hechas) bloque = paginas - hechas;
        libres[nodo] -= bloque;

        if (n > 0 && ntohs(tr[n - 1].hogar) == nodo)     /* juntar tramos seguidos */
            tr[n - 1].cantidad = htonl(ntohl(tr[n - 1].cantidad) + bloque);
        else if (n == max) return DSM_ENOMEM;
        else tr[n++] = (tramo_t){ .vpn_inicio = htonl(vpn_base + hechas),
                                  .cantidad = htonl(bloque), .hogar = htons(nodo) };
        hechas += bloque;
        nodo = (nodo + 1) % MAX_NODOS;
    }
    return n;
}

void atender_malloc(mensaje_t *m)
{
    uint64_t tam = m->loc.valor;
    int politica = m->loc.acceso > 1 ? g.cfg.politica_defecto : m->loc.acceso;
    m->loc.nombre[DSM_MAX_NOMBRE - 1] = 0;
    if (tam == 0 || !m->loc.nombre[0]) { red_responder_error(m, DSM_EINVAL); mensaje_liberar(m); return; }
    if (segmento_por_nombre(m->loc.nombre)) { red_responder_error(m, DSM_EEXISTE); mensaje_liberar(m); return; }

    uint32_t paginas = (tam + g.cfg.tam_pagina - 1) / g.cfg.tam_pagina;
    pthread_mutex_lock(&g.mx_segs);
    uint32_t base = g.sig_vpn;
    int cabe = base + (uint64_t)paginas <= (uint64_t)(g.yo + 1) * g.paginas_ventana;
    if (cabe) g.sig_vpn += paginas;
    static uint32_t contador = 0;
    uint32_t id = ((uint32_t)g.yo << 20) | ++contador;
    pthread_mutex_unlock(&g.mx_segs);
    if (!cabe) { red_responder_error(m, DSM_ENOMEM); mensaje_liberar(m); return; }

    uint8_t *buf = malloc(sizeof(msg_segment_create_t) + MAX_TRAMOS * sizeof(tramo_t));
    msg_segment_create_t *sc = (msg_segment_create_t *)buf;
    tramo_t *tr = (tramo_t *)(sc + 1);
    int n = elegir_hogares(base, paginas, politica, tr, MAX_TRAMOS);
    if (n < 0) { red_responder_error(m, n); free(buf); mensaje_liberar(m); return; }

    memset(sc, 0, sizeof *sc);
    sc->id = htonl(id);
    sc->vpn_base = htonl(base);
    sc->paginas = htonl(paginas);
    sc->tam_bytes = htobe64(tam);
    sc->creador = htons(g.yo);
    sc->cantidad_tramos = htons(n);
    sc->pid_creador = htonl(m->loc.pid);
    memcpy(sc->nombre, m->loc.nombre, DSM_MAX_NOMBRE);

    int err = registrar_segmento(sc, tr);
    if (!err) {
        /* el puntero se entrega hasta que todos los nodos conocen el segmento */
        int otros = 0;
        for (int i = 0; i < MAX_NODOS; i++)
            if (i != g.yo && g.nodos[i].en_linea) otros++;
        uint32_t seq = pend_nueva(otros);
        uint32_t len = sizeof *sc + n * sizeof(tramo_t);
        for (int i = 0; i < MAX_NODOS; i++)
            if (i != g.yo && g.nodos[i].en_linea &&
                red_enviar(i, MSG_SEGMENT_CREATE, 0, seq, buf, len, NULL))
                pend_completar(seq, 0, NULL);
        err = pend_esperar(seq, NULL);
    }
    free(buf);
    if (err) red_responder_error(m, err);
    else {
        responder_app(m->fd, m->loc.id, 0, g.cfg.base_global + (uint64_t)base * g.cfg.tam_pagina, tam);
        LOG("segmento '%s' creado: %u paginas en %d tramos", m->loc.nombre, paginas, n);
    }
    mensaje_liberar(m);
}

void atender_segment_create(mensaje_t *m)
{
    int err = DSM_EPROTOCOLO;
    msg_segment_create_t *sc = (msg_segment_create_t *)m->cuerpo;
    if (m->len >= sizeof *sc &&
        m->len >= sizeof *sc + ntohs(sc->cantidad_tramos) * sizeof(tramo_t))
        err = registrar_segmento(sc, (tramo_t *)(sc + 1));
    if (err) red_responder_error(m, err);
    else {
        msg_segment_ack_t a = { .id = sc->id };
        red_enviar(m->cab.origen, MSG_SEGMENT_ACK, BANDERA_RESP, m->cab.secuencia, &a, sizeof a, NULL);
    }
    mensaje_liberar(m);
}

/* A un nodo que acaba de entrar se le dan a conocer los segmentos creados aqui */
void enviar_segmentos_a(uint16_t id)
{
    pthread_mutex_lock(&g.mx_segs);
    for (int i = 0; i < g.n_segs; i++) {
        segmento_t *s = g.segs[i];
        if (s->creador != g.yo) continue;
        uint8_t *buf = malloc(sizeof(msg_segment_create_t) + s->n_tramos * sizeof(tramo_t));
        uint32_t len = armar_segmento(s, buf);
        red_enviar(id, MSG_SEGMENT_CREATE, 0, 0, buf, len, NULL);
        free(buf);
    }
    pthread_mutex_unlock(&g.mx_segs);
}

/* membresia.c - ingreso de nodos, tabla de nodos y latidos.
 * Prototipo 2: JOIN desde cualquier semilla (JOIN_REDIRECT) y estado sospechoso. */
#include <arpa/inet.h>
#include <endian.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "dsmd.h"

static uint16_t puerto_de(uint16_t id) { return g.cfg.puerto_minimo + 2 * id; }

static void llenar_entrada(uint16_t id, entrada_nodo_t *e)
{
    nodo_t *n = &g.nodos[id];
    memset(e, 0, sizeof *e);
    e->id = htons(id);
    e->estado = !n->en_linea ? NODO_CAIDO : n->sospechoso ? NODO_SOSPECHOSO : NODO_EN_LINEA;
    e->ip = n->ip;
    e->puerto_datos = htons(n->puerto);
    e->puerto_monitor = htons(n->puerto + 1);
    e->paginas = htonl(n->paginas);
    e->libres = htonl(n->libres);
}

static void registrar(const entrada_nodo_t *e, int fd)
{
    uint16_t id = ntohs(e->id);
    if (id >= MAX_NODOS || id == g.yo) return;
    nodo_t *n = &g.nodos[id];
    pthread_mutex_lock(&g.mx_nodos);
    n->ip = e->ip;
    n->puerto = ntohs(e->puerto_datos);
    n->paginas = ntohl(e->paginas);
    n->libres = ntohl(e->libres);
    n->ultimo_ms = ahora_ms();
    if (fd >= 0 || !n->en_uso) n->fd = fd;
    n->en_uso = 1;
    n->en_linea = (e->estado != NODO_CAIDO);
    n->sospechoso = 0;
    pthread_mutex_unlock(&g.mx_nodos);
}

static void registrar_yo(uint32_t ip)
{
    nodo_t *n = &g.nodos[g.yo];
    n->en_uso = n->en_linea = 1;
    n->ip = ip;
    n->puerto = puerto_de(g.yo);
    n->paginas = n->libres = g.cfg.memoria_por_nodo / g.cfg.tam_pagina;
    n->fd = -1;
}

int membresia_fundar(void)
{
    g.yo = 0;
    g.fd_escucha = red_escuchar(puerto_de(0));
    if (g.fd_escucha < 0) {
        fprintf(stderr, "dsmd: el puerto %u esta ocupado\n", puerto_de(0));
        return DSM_ERED;
    }
    registrar_yo(0);
    LOG("nodo fundador, escuchando en el puerto %u", puerto_de(0));
    return DSM_OK;
}

static int conectar_a(const char *host, uint16_t puerto, uint32_t *ip)
{
    struct addrinfo pista = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, NULL, &pista, &res) != 0) return -1;
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return red_conectar(*ip, puerto);
}

static int conectar_semilla(const char *s, uint32_t *ip)
{
    char host[64];
    snprintf(host, sizeof host, "%s", s);
    char *dos_puntos = strrchr(host, ':');
    if (!dos_puntos) return -1;
    *dos_puntos = 0;
    return conectar_a(host, atoi(dos_puntos + 1), ip);
}

int membresia_unirse(void)
{
    int fd = -1;
    uint32_t ip_semilla = 0;
    for (int i = 0; i < g.cfg.n_semillas && fd < 0; i++)
        fd = conectar_semilla(g.cfg.semilla[i], &ip_semilla);
    if (fd < 0) {
        fprintf(stderr, "dsmd: ninguna semilla responde; use -f para fundar el cluster\n");
        return DSM_ERED;
    }

    /* la IP con la que este nodo se anuncia es la de la interfaz que llega a la semilla */
    struct sockaddr_in yo;
    socklen_t l = sizeof yo;
    getsockname(fd, (struct sockaddr *)&yo, &l);

    msg_join_t j = { .ip = yo.sin_addr.s_addr,
                     .paginas = htonl(g.cfg.memoria_por_nodo / g.cfg.tam_pagina),
                     .hash_conf = htonl(g.cfg.hash) };
    gethostname(j.nombre, sizeof j.nombre - 1);
    red_enviar_fd(fd, MSG_JOIN, 0, 1, &j, sizeof j, NULL);

    /* Si la semilla no es el coordinador, responde con su direccion y se
     * repite el JOIN ahi (a lo sumo unas cuantas veces). */
    mensaje_t *m = red_leer(fd);
    for (int saltos = 0; m && m->cab.tipo == MSG_JOIN_REDIRECT && saltos < 4; saltos++) {
        entrada_nodo_t c = *(entrada_nodo_t *)m->cuerpo;
        mensaje_liberar(m);
        m = NULL;
        close(fd);
        char ip[INET_ADDRSTRLEN];
        if (c.ip) inet_ntop(AF_INET, &c.ip, ip, sizeof ip);
        else inet_ntop(AF_INET, &ip_semilla, ip, sizeof ip);   /* misma maquina que la semilla */
        LOG("la semilla no es el coordinador; se pide el ingreso al nodo %u (%s:%u)",
            ntohs(c.id), ip, ntohs(c.puerto_datos));
        fd = conectar_a(ip, ntohs(c.puerto_datos), &ip_semilla);
        if (fd < 0) break;
        l = sizeof yo;
        getsockname(fd, (struct sockaddr *)&yo, &l);
        j.ip = yo.sin_addr.s_addr;
        red_enviar_fd(fd, MSG_JOIN, 0, 1, &j, sizeof j, NULL);
        m = red_leer(fd);
    }
    if (!m || m->cab.tipo != MSG_JOIN_ACK) {
        int cod = (m && m->cab.tipo == MSG_ERROR) ? (int32_t)ntohl(((msg_error_t *)m->cuerpo)->codigo)
                                                  : DSM_EPROTOCOLO;
        fprintf(stderr, "dsmd: el ingreso fue rechazado: %s\n", cod == DSM_ECONF
                ? "el dsm.conf de este nodo no coincide con el del cluster" : dsm_strerror(cod));
        mensaje_liberar(m);
        if (fd >= 0) close(fd);
        return cod;
    }

    msg_join_ack_t *a = (msg_join_ack_t *)m->cuerpo;
    entrada_nodo_t *e = (entrada_nodo_t *)(a + 1);
    uint16_t coord = m->cab.origen;
    g.yo = ntohs(a->id_asignado);
    registrar_yo(yo.sin_addr.s_addr);
    for (int i = 0; i < ntohs(a->cantidad); i++) {
        if (ntohs(e[i].id) == coord && e[i].ip == 0) e[i].ip = ip_semilla;
        registrar(&e[i], ntohs(e[i].id) == coord ? fd : -1);
    }
    mensaje_liberar(m);

    g.fd_escucha = red_escuchar(puerto_de(g.yo));
    if (g.fd_escucha < 0) {
        fprintf(stderr, "dsmd: el puerto %u esta ocupado\n", puerto_de(g.yo));
        return DSM_ERED;
    }

    /* abrir una conexion con cada uno de los demas nodos */
    entrada_nodo_t mia;
    llenar_entrada(g.yo, &mia);
    for (int i = 0; i < MAX_NODOS; i++) {
        nodo_t *n = &g.nodos[i];
        if (i == g.yo || i == coord || !n->en_uso || !n->en_linea) continue;
        n->fd = red_conectar(n->ip, n->puerto);
        if (n->fd < 0) { n->en_linea = 0; continue; }
        red_enviar_fd(n->fd, MSG_CONNECT, 0, 0, &mia, sizeof mia, NULL);
    }
    LOG("ingreso al cluster como nodo %u, puerto %u", g.yo, puerto_de(g.yo));
    return DSM_OK;
}

/* Solo el coordinador (nodo en linea de menor id) asigna identificadores */
void atender_join(mensaje_t *m)
{
    msg_join_t *j = (msg_join_t *)m->cuerpo;
    int cod = DSM_OK;
    if (m->len < sizeof *j) cod = DSM_EPROTOCOLO;
    else if (ntohl(j->hash_conf) != g.cfg.hash) cod = DSM_ECONF;
    else if (coordinador() != g.yo) {
        /* Prototipo 2: este nodo no asigna ids; le indica al nuevo a quien pedirlo */
        entrada_nodo_t c;
        llenar_entrada(coordinador(), &c);
        red_enviar_fd(m->fd, MSG_JOIN_REDIRECT, BANDERA_RESP, m->cab.secuencia, &c, sizeof c, NULL);
        close(m->fd);
        mensaje_liberar(m);
        return;
    }

    int id = -1;
    pthread_mutex_lock(&g.mx_nodos);
    for (int i = 0; i < MAX_NODOS && cod == DSM_OK && id < 0; i++)
        if (!g.nodos[i].en_uso) id = i;
    if (cod == DSM_OK && id < 0) cod = DSM_ENOMEM;
    if (cod == DSM_OK) {
        nodo_t *n = &g.nodos[id];
        n->en_uso = n->en_linea = 1;
        n->ip = j->ip;
        n->puerto = puerto_de(id);
        n->paginas = n->libres = ntohl(j->paginas);
        n->ultimo_ms = ahora_ms();
        n->fd = m->fd;
    }
    pthread_mutex_unlock(&g.mx_nodos);

    if (cod != DSM_OK) {
        msg_error_t e = { .codigo = htonl(cod), .tipo = htons(MSG_JOIN) };
        red_enviar_fd(m->fd, MSG_ERROR, BANDERA_RESP, m->cab.secuencia, &e, sizeof e, NULL);
        close(m->fd);
        LOG("ingreso rechazado: %s", dsm_strerror(cod));
        mensaje_liberar(m);
        return;
    }

    int cant = 0;
    uint8_t buf[sizeof(msg_join_ack_t) + MAX_NODOS * sizeof(entrada_nodo_t)];
    entrada_nodo_t *e = (entrada_nodo_t *)(buf + sizeof(msg_join_ack_t));
    for (int i = 0; i < MAX_NODOS; i++)
        if (g.nodos[i].en_uso) llenar_entrada(i, &e[cant++]);
    *(msg_join_ack_t *)buf = (msg_join_ack_t){ .id_asignado = htons(id), .cantidad = htons(cant) };
    red_enviar(id, MSG_JOIN_ACK, BANDERA_RESP, m->cab.secuencia, buf,
               sizeof(msg_join_ack_t) + cant * sizeof(entrada_nodo_t), NULL);

    entrada_nodo_t nueva;
    llenar_entrada(id, &nueva);
    for (int i = 0; i < MAX_NODOS; i++)
        if (i != g.yo && i != id && g.nodos[i].en_linea)
            red_enviar(i, MSG_NODE_ANNOUNCE, 0, 0, &nueva, sizeof nueva, NULL);

    red_despertar();
    enviar_segmentos_a(id);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &j->ip, ip, sizeof ip);
    LOG("nodo %d se unio desde %s (%s), aporta %u paginas", id, ip, j->nombre, ntohl(j->paginas));
    mensaje_liberar(m);
}

/* Un nodo nuevo abrio una conexion con este nodo y se presenta */
void atender_connect(mensaje_t *m)
{
    if (m->len < sizeof(entrada_nodo_t)) { close(m->fd); mensaje_liberar(m); return; }
    entrada_nodo_t *e = (entrada_nodo_t *)m->cuerpo;
    registrar(e, m->fd);
    red_despertar();
    enviar_segmentos_a(ntohs(e->id));
    LOG("conexion establecida con el nodo %u", ntohs(e->id));
    mensaje_liberar(m);
}

void atender_announce(mensaje_t *m)
{
    if (m->len >= sizeof(entrada_nodo_t)) registrar((entrada_nodo_t *)m->cuerpo, -1);
    mensaje_liberar(m);
}

void atender_heartbeat(mensaje_t *m)
{
    uint16_t o = m->cab.origen;
    if (o >= MAX_NODOS || m->len < sizeof(msg_heartbeat_t)) return;
    msg_heartbeat_t *h = (msg_heartbeat_t *)m->cuerpo;
    pthread_mutex_lock(&g.mx_nodos);
    g.nodos[o].libres = ntohl(h->libres);
    pthread_mutex_unlock(&g.mx_nodos);
}

void nodo_caido(uint16_t id)
{
    pthread_mutex_lock(&g.mx_nodos);
    nodo_t *n = &g.nodos[id];
    int estaba = n->en_linea;
    n->en_linea = 0;
    if (estaba && n->fd >= 0) shutdown(n->fd, SHUT_RDWR);
    pthread_mutex_unlock(&g.mx_nodos);
    if (estaba) {
        LOG("el nodo %u se dio por caido", id);
        sincro_nodo_caido(id);
    }
}

uint16_t coordinador(void)
{
    for (int i = 0; i < MAX_NODOS; i++)
        if (g.nodos[i].en_linea) return i;
    return g.yo;
}

void *hilo_latidos(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(g.cfg.intervalo_latido * 1000);
        msg_heartbeat_t h = { .marca_ms = htobe64(ahora_ms()), .libres = htonl(g.nodos[g.yo].libres),
                              .copias = htonl(cache_ocupadas()) };
        for (int i = 0; i < MAX_NODOS; i++) {
            if (i == g.yo || !g.nodos[i].en_linea) continue;
            red_enviar(i, MSG_HEARTBEAT, 0, 0, &h, sizeof h, NULL);
            /* la comparacion se hace con la hora actual: el escuchador pudo
             * actualizar ultimo_ms mientras se enviaba */
            uint64_t ahora = ahora_ms(), ultimo = g.nodos[i].ultimo_ms;
            if (ahora > ultimo + g.cfg.caida_tras) nodo_caido(i);
            else {
                int sosp = ahora > ultimo + g.cfg.sospecha_tras;
                if (sosp && !g.nodos[i].sospechoso)
                    LOG("el nodo %u no envia mensajes hace %llu ms: sospechoso", i,
                        (unsigned long long)(ahora - ultimo));
                if (!sosp && g.nodos[i].sospechoso) LOG("el nodo %u volvio a responder", i);
                g.nodos[i].sospechoso = sosp;
            }
        }
    }
    return NULL;
}

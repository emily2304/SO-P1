/* network.c - sockets TCP entre nodos: envio, lectura y el hilo escuchador */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "dsmd.h"

static void sin_retraso(int fd)
{
    int uno = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
}

int red_escuchar(uint16_t puerto)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), uno = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(puerto),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 32) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int red_conectar(uint32_t ip, uint16_t puerto)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(puerto),
                             .sin_addr.s_addr = ip };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    sin_retraso(fd);
    return fd;
}

static int enviar_todo(int fd, struct iovec *v, int n)
{
    struct msghdr mh = { .msg_iov = v, .msg_iovlen = n };
    while (mh.msg_iovlen > 0) {
        ssize_t r = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        while (r > 0 && mh.msg_iovlen > 0) {     /* avanzar sobre lo ya enviado */
            if ((size_t)r >= mh.msg_iov->iov_len) {
                r -= mh.msg_iov->iov_len;
                mh.msg_iov++;
                mh.msg_iovlen--;
            } else {
                mh.msg_iov->iov_base = (char *)mh.msg_iov->iov_base + r;
                mh.msg_iov->iov_len -= r;
                r = 0;
            }
        }
    }
    return 0;
}

static int enviar_crudo(int fd, uint16_t dest, uint16_t tipo, uint8_t banderas, uint32_t seq,
                        const void *cuerpo, uint32_t len, const void *pagina)
{
    uint32_t extra = pagina ? g.cfg.tam_pagina : 0;
    cabecera_t c = {
        .magico = htonl(DSM_MAGICO), .version = DSM_VERSION, .banderas = banderas,
        .tipo = htons(tipo), .origen = htons(g.yo), .destino = htons(dest),
        .secuencia = htonl(seq), .longitud = htonl(len + extra), .reservado = 0
    };
    struct iovec v[3] = {
        { &c, sizeof c }, { (void *)cuerpo, len }, { (void *)pagina, extra }
    };
    SUMAR(mensajes_enviados, 1);
    if (pagina) SUMAR(paginas_enviadas, 1);
    return enviar_todo(fd, v, pagina ? 3 : 2);
}

int red_enviar_fd(int fd, uint16_t tipo, uint8_t banderas, uint32_t seq,
                  const void *cuerpo, uint32_t len, const void *pagina)
{
    return enviar_crudo(fd, NODO_TODOS, tipo, banderas, seq, cuerpo, len, pagina);
}

/* Cada conexion tiene su mutex: un mensaje se escribe completo antes que otro */
int red_enviar(uint16_t dest, uint16_t tipo, uint8_t banderas, uint32_t seq,
               const void *cuerpo, uint32_t len, const void *pagina)
{
    if (dest >= MAX_NODOS) return DSM_EINVAL;
    nodo_t *n = &g.nodos[dest];
    pthread_mutex_lock(&n->envio);
    if (!n->en_linea || n->fd < 0) {
        pthread_mutex_unlock(&n->envio);
        return DSM_ENODOCAIDO;
    }
    int r = enviar_crudo(n->fd, dest, tipo, banderas, seq, cuerpo, len, pagina);
    pthread_mutex_unlock(&n->envio);
    if (r < 0) {
        nodo_caido(dest);
        return DSM_ERED;
    }
    return DSM_OK;
}

static int leer_todo(int fd, void *buf, size_t n)
{
    size_t hecho = 0;
    while (hecho < n) {
        ssize_t r = recv(fd, (char *)buf + hecho, n - hecho, 0);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        hecho += r;
    }
    return 0;
}

/* Lee un mensaje completo: primero la cabecera y luego 'longitud' bytes */
mensaje_t *red_leer(int fd)
{
    cabecera_t c;
    if (leer_todo(fd, &c, sizeof c) < 0) return NULL;
    if (ntohl(c.magico) != DSM_MAGICO || c.version != DSM_VERSION) return NULL;

    mensaje_t *m = calloc(1, sizeof *m);
    m->cab = c;
    m->cab.tipo = ntohs(c.tipo);
    m->cab.origen = ntohs(c.origen);
    m->cab.destino = ntohs(c.destino);
    m->cab.secuencia = ntohl(c.secuencia);
    m->cab.longitud = ntohl(c.longitud);
    m->len = m->cab.longitud;
    m->fd = fd;
    if (m->len > (1u << 20)) { free(m); return NULL; }
    if (m->len) {
        m->cuerpo = malloc(m->len);
        if (leer_todo(fd, m->cuerpo, m->len) < 0) { mensaje_liberar(m); return NULL; }
    }
    SUMAR(mensajes_recibidos, 1);
    return m;
}

void red_responder_error(const mensaje_t *m, int codigo)
{
    if (m->local) {
        responder_app(m->fd, m->loc.id, codigo, 0, 0);
        return;
    }
    msg_error_t e = { .codigo = htonl(codigo), .tipo = htons(m->cab.tipo) };
    red_enviar(m->cab.origen, MSG_ERROR, BANDERA_RESP, m->cab.secuencia, &e, sizeof e, NULL);
}

void red_despertar(void)
{
    char c = 1;
    if (write(g.tubo[1], &c, 1) < 0) { }
}

/* Reparte cada mensaje segun su tipo. Las respuestas van directo a quien las
 * espera, salvo PAGE_DATA, que debe aplicarse en orden con INVALIDATE y FETCH. */
static void clasificar(mensaje_t *m)
{
    uint16_t t = m->cab.tipo;
    if (m->cab.origen < MAX_NODOS)          /* cualquier mensaje cuenta como senal de vida */
        g.nodos[m->cab.origen].ultimo_ms = ahora_ms();
    if (t == MSG_HEARTBEAT) {
        atender_heartbeat(m);
        mensaje_liberar(m);
    } else if ((m->cab.banderas & BANDERA_RESP) && t != MSG_PAGE_DATA) {
        int err = 0;
        if (t == MSG_ERROR && m->len >= sizeof(msg_error_t))
            err = (int32_t)ntohl(((msg_error_t *)m->cuerpo)->codigo);
        pend_completar(m->cab.secuencia, err, err ? NULL : m);
        if (err) mensaje_liberar(m);
    } else if (t == MSG_PAGE_REQUEST || t == MSG_WRITEBACK) {
        encolar(&g.peticiones, m);
    } else {
        encolar(&g.ordenes, m);
    }
}

#define MAX_SUELTAS 32

void *hilo_escuchador(void *arg)
{
    (void)arg;
    int sueltas[MAX_SUELTAS], n_sueltas = 0;    /* conexiones que aun no se presentan */

    for (;;) {
        struct pollfd pf[2 + MAX_NODOS + MAX_SUELTAS];
        int ids[MAX_NODOS], n = 0;
        pf[n++] = (struct pollfd){ .fd = g.tubo[0], .events = POLLIN };
        pf[n++] = (struct pollfd){ .fd = g.fd_escucha, .events = POLLIN };
        pthread_mutex_lock(&g.mx_nodos);
        int nn = 0;
        for (int i = 0; i < MAX_NODOS; i++)
            if (i != g.yo && g.nodos[i].en_uso && g.nodos[i].fd >= 0) {
                ids[nn++] = i;
                pf[n++] = (struct pollfd){ .fd = g.nodos[i].fd, .events = POLLIN };
            }
        pthread_mutex_unlock(&g.mx_nodos);
        int base_sueltas = n;
        for (int i = 0; i < n_sueltas; i++)
            pf[n++] = (struct pollfd){ .fd = sueltas[i], .events = POLLIN };

        if (poll(pf, n, 1000) <= 0) continue;

        if (pf[0].revents) {
            char b[64];
            if (read(g.tubo[0], b, sizeof b) < 0) { }
        }
        if ((pf[1].revents & POLLIN) && n_sueltas < MAX_SUELTAS) {
            int fd = accept(g.fd_escucha, NULL, NULL);
            if (fd >= 0) {
                sin_retraso(fd);
                sueltas[n_sueltas++] = fd;
            }
        }
        for (int i = 0; i < nn; i++) {
            if (!pf[2 + i].revents) continue;
            mensaje_t *m = red_leer(pf[2 + i].fd);
            if (m) { clasificar(m); continue; }
            nodo_t *nd = &g.nodos[ids[i]];          /* conexion cerrada */
            nodo_caido(ids[i]);
            pthread_mutex_lock(&nd->envio);
            if (nd->fd == pf[2 + i].fd) { close(nd->fd); nd->fd = -1; }
            pthread_mutex_unlock(&nd->envio);
        }
        for (int i = n - 1; i >= base_sueltas; i--) {
            if (!pf[i].revents) continue;
            int fd = pf[i].fd;
            sueltas[i - base_sueltas] = sueltas[--n_sueltas];
            mensaje_t *m = red_leer(fd);
            if (m && m->cab.tipo == MSG_JOIN) encolar(&g.ordenes, m);
            else if (m && m->cab.tipo == MSG_CONNECT) atender_connect(m);
            else { mensaje_liberar(m); close(fd); }
        }
    }
    return NULL;
}

/* clientes.c - socket Unix con las aplicaciones locales y con dsmctl */
#include <arpa/inet.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "dsmd.h"

static void enviar_local(int fd, const msg_local_t *m)
{
    if (send(fd, m, sizeof *m, MSG_NOSIGNAL) < 0) { }
}

void responder_app(int fd, uint32_t id, int error, uint64_t valor, uint64_t valor2)
{
    msg_local_t r = { .tipo = LOC_RESPUESTA, .error = error, .id = id, .valor = valor, .valor2 = valor2 };
    enviar_local(fd, &r);
}

void conceder(int fd, uint32_t ranura, uint32_t vpn, int prot, int error)
{
    msg_local_t r = { .tipo = LOC_CONCESION, .error = error, .vpn = vpn, .ranura = ranura, .acceso = prot };
    enviar_local(fd, &r);
}

/* Baja el permiso de una pagina en todos los procesos del nodo y espera que confirmen */
int revocar_locales(uint32_t vpn, int prot)
{
    pthread_mutex_lock(&g.mx_procs);
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (g.procs[i].pid) n++;
    uint32_t seq = pend_nueva(n);
    msg_local_t r = { .tipo = LOC_REVOCAR, .id = seq, .vpn = vpn, .acceso = prot };
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &g.procs[i];
        if (!p->pid) continue;
        if (p->n_debe < MAX_DEBE && send(p->fd, &r, sizeof r, MSG_NOSIGNAL) == (ssize_t)sizeof r)
            p->debe[p->n_debe++] = seq;
        else
            pend_completar(seq, 0, NULL);
    }
    pthread_mutex_unlock(&g.mx_procs);
    return pend_esperar(seq, NULL);
}

/* dsm_free: baja a PROT_NONE todo un rango de paginas con un solo mensaje */
int revocar_rango(uint32_t vpn, uint32_t paginas)
{
    pthread_mutex_lock(&g.mx_procs);
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (g.procs[i].pid) n++;
    uint32_t seq = pend_nueva(n);
    msg_local_t r = { .tipo = LOC_REVOCAR, .id = seq, .vpn = vpn, .acceso = PROT_NONE, .valor2 = paginas };
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &g.procs[i];
        if (!p->pid) continue;
        if (p->n_debe < MAX_DEBE && send(p->fd, &r, sizeof r, MSG_NOSIGNAL) == (ssize_t)sizeof r)
            p->debe[p->n_debe++] = seq;
        else
            pend_completar(seq, 0, NULL);
    }
    pthread_mutex_unlock(&g.mx_procs);
    return pend_esperar(seq, NULL);
}

static void bienvenida(int fd, const msg_local_t *hola)
{
    msg_local_t r = { .tipo = LOC_BIENVENIDA, .id = hola->id, .vpn = g.cfg.tam_pagina,
                      .ranura = g.yo, .valor = g.cfg.base_global, .valor2 = g.tam_espacio };
    char ctrl[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec v = { &r, sizeof r };
    struct msghdr mh = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = ctrl, .msg_controllen = sizeof ctrl };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;           /* el memfd viaja al proceso de la aplicacion */
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &g.memfd, sizeof(int));
    if (sendmsg(fd, &mh, MSG_NOSIGNAL) < 0) { }
}

/* Las consultas se arman en monitor.c. Una consulta global espera respuestas
 * de otros nodos, asi que se atiende en un hilo aparte para no detener a este,
 * que tambien recibe los fallos y las confirmaciones de las aplicaciones. */
typedef struct { int fd; msg_local_t m; } consulta_t;

static void *hilo_consulta(void *arg)
{
    consulta_t *c = arg;
    texto_t t = { 0 };
    agregar(&t, "%s", "");
    c->m.nombre[DSM_MAX_NOMBRE - 1] = 0;
    consulta_texto(c->m.nombre, c->m.valor == 1, &t);

    msg_local_t r = { .tipo = LOC_TEXTO, .id = c->m.id, .valor = t.n };
    enviar_local(c->fd, &r);
    for (size_t hecho = 0; hecho < t.n; hecho += 32768) {
        size_t n = t.n - hecho < 32768 ? t.n - hecho : 32768;
        if (send(c->fd, t.s + hecho, n, MSG_NOSIGNAL) < 0) break;
    }
    free(t.s);
    free(c);
    return NULL;
}

static void consulta(int fd, const msg_local_t *m)
{
    consulta_t *c = malloc(sizeof *c);
    c->fd = fd;
    c->m = *m;
    if (m->valor == 1) {
        pthread_t h;
        if (pthread_create(&h, NULL, hilo_consulta, c) == 0) { pthread_detach(h); return; }
    }
    hilo_consulta(c);
}

static void info(int fd, const msg_local_t *m)
{
    msg_local_t r = { .tipo = LOC_RESPUESTA, .id = m->id, .vpn = g.cfg.tam_pagina, .ranura = g.yo };
    pthread_mutex_lock(&g.mx_nodos);
    for (int i = 0; i < MAX_NODOS; i++)
        if (g.nodos[i].en_linea) {
            r.pid++;
            r.valor += (uint64_t)g.nodos[i].paginas * g.cfg.tam_pagina;
            r.valor2 += (uint64_t)g.nodos[i].libres * g.cfg.tam_pagina;
        }
    pthread_mutex_unlock(&g.mx_nodos);
    enviar_local(fd, &r);
}

static void atender(int fd, int i, const msg_local_t *m)
{
    if (m->tipo == LOC_HOLA) {
        pthread_mutex_lock(&g.mx_procs);
        g.procs[i].pid = m->pid;
        pthread_mutex_unlock(&g.mx_procs);
        bienvenida(fd, m);
    } else if (m->tipo == LOC_FALLO || m->tipo == LOC_MALLOC || m->tipo == LOC_FREE ||
               m->tipo == LOC_BARRERA || m->tipo == LOC_CANDADO || m->tipo == LOC_SOLTAR) {
        mensaje_t *msg = calloc(1, sizeof *msg);
        msg->local = 1;
        msg->fd = fd;
        msg->loc = *m;
        /* barreras y candados no esperan a nadie: van al hilo de ordenes */
        int sincro = m->tipo == LOC_BARRERA || m->tipo == LOC_CANDADO || m->tipo == LOC_SOLTAR;
        encolar(sincro ? &g.ordenes : &g.locales, msg);
    } else if (m->tipo == LOC_REVOCADO) {
        pthread_mutex_lock(&g.mx_procs);
        proc_t *p = &g.procs[i];
        for (int k = 0; k < p->n_debe; k++)
            if (p->debe[k] == m->id) { p->debe[k] = p->debe[--p->n_debe]; break; }
        pthread_mutex_unlock(&g.mx_procs);
        pend_completar(m->id, 0, NULL);
    } else if (m->tipo == LOC_BUSCAR) {
        char nombre[DSM_MAX_NOMBRE];
        memcpy(nombre, m->nombre, DSM_MAX_NOMBRE);
        nombre[DSM_MAX_NOMBRE - 1] = 0;
        segmento_t *s = segmento_por_nombre(nombre);
        if (s) segmento_registrar_pid(s, m->pid);
        if (s) responder_app(fd, m->id, 0, g.cfg.base_global + (uint64_t)s->vpn_base * g.cfg.tam_pagina, s->tam_bytes);
        else responder_app(fd, m->id, DSM_ENOENT, 0, 0);
    } else if (m->tipo == LOC_INFO) {
        info(fd, m);
    } else if (m->tipo == LOC_CONSULTA) {
        consulta(fd, m);
    }
}

void *hilo_clientes(void *arg)
{
    (void)arg;
    int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", g.ruta_socket);
    unlink(g.ruta_socket);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0) {
        LOG("no se pudo abrir el socket local %s", g.ruta_socket);
        exit(1);
    }
    for (int i = 0; i < MAX_PROCS; i++) g.procs[i].fd = -1;

    for (;;) {
        struct pollfd pf[MAX_PROCS + 1];
        int idx[MAX_PROCS], n = 1;
        pf[0] = (struct pollfd){ .fd = ls, .events = POLLIN };
        for (int i = 0; i < MAX_PROCS; i++)
            if (g.procs[i].fd >= 0) {
                idx[n - 1] = i;
                pf[n++] = (struct pollfd){ .fd = g.procs[i].fd, .events = POLLIN };
            }
        if (poll(pf, n, -1) <= 0) continue;

        if (pf[0].revents & POLLIN) {
            int fd = accept(ls, NULL, NULL);
            pthread_mutex_lock(&g.mx_procs);
            int i = 0;
            while (i < MAX_PROCS && g.procs[i].fd >= 0) i++;
            if (i < MAX_PROCS) g.procs[i] = (proc_t){ .fd = fd, .pid = 0 };
            else close(fd);
            pthread_mutex_unlock(&g.mx_procs);
        }
        for (int k = 1; k < n; k++) {
            if (!pf[k].revents) continue;
            int i = idx[k - 1];
            msg_local_t m;
            ssize_t r = recv(pf[k].fd, &m, sizeof m, 0);
            if (r == (ssize_t)sizeof m) { atender(pf[k].fd, i, &m); continue; }
            /* el proceso termino: lo que no confirmo ya no hace falta esperarlo */
            pthread_mutex_lock(&g.mx_procs);
            proc_t p = g.procs[i];
            close(p.fd);
            g.procs[i] = (proc_t){ .fd = -1, .pid = 0 };
            pthread_mutex_unlock(&g.mx_procs);
            for (int k = 0; k < p.n_debe; k++) pend_completar(p.debe[k], 0, NULL);
        }
    }
    return NULL;
}

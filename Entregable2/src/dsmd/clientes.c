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

static const char *NOMBRE_EST[] = { "I", "S", "M", "IS", "IM", "SM" };

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

/* ---------- consultas en texto para dsmctl ---------- */
typedef struct { char *s; size_t n, cap; } texto_t;

static void agregar(texto_t *t, const char *fmt, ...)
{
    va_list ap;
    if (t->cap - t->n < 512) t->s = realloc(t->s, t->cap = t->cap * 2 + 4096);
    va_start(ap, fmt);
    t->n += vsnprintf(t->s + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
}

static void consulta_nodos(texto_t *t)
{
    uint64_t ahora = ahora_ms();
    agregar(t, "Tabla de nodos (este nodo: %u, coordinador: %u)\n", g.yo, coordinador());
    agregar(t, "%-4s %-9s %-16s %-7s %-9s %-9s %s\n", "ID", "ESTADO", "IP", "PUERTO", "PAGINAS", "LIBRES", "ULTIMO LATIDO");
    pthread_mutex_lock(&g.mx_nodos);
    for (int i = 0; i < MAX_NODOS; i++) {
        nodo_t *n = &g.nodos[i];
        if (!n->en_uso) continue;
        char ip[INET_ADDRSTRLEN] = "-", lat[32] = "-";
        if (n->ip) inet_ntop(AF_INET, &n->ip, ip, sizeof ip);
        if (i != g.yo) snprintf(lat, sizeof lat, "hace %llu ms", (unsigned long long)(ahora - n->ultimo_ms));
        agregar(t, "%-4d %-9s %-16s %-7u %-9u %-9u %s\n", i, n->en_linea ? "en linea" : "caido",
                ip, n->puerto, n->paginas, n->libres, i == g.yo ? "(este nodo)" : lat);
    }
    pthread_mutex_unlock(&g.mx_nodos);
}

static void consulta_paginas(texto_t *t)
{
    agregar(t, "Tabla de paginas del nodo %u (paginas que este nodo ha usado o de las que es hogar)\n", g.yo);
    agregar(t, "%-9s %-16s %-6s %-6s %-6s %-12s %-8s %s\n", "VPN", "DIRECCION", "ESTADO", "HOGAR", "DUENO", "COPIAS", "FALLOS", "SEGMENTO");
    int lineas = 0;
    for (int f = 0; f < NUM_FRANJAS; f++) {
        pthread_mutex_lock(&g.franjas[f].mutex);
        for (int c = f; c < NUM_CUBETAS; c += NUM_FRANJAS)
            for (entrada_pagina_t *e = g.cubetas[c]; e && lineas < 20000; e = e->sig, lineas++) {
                segmento_t *s = segmento_de_vpn(e->vpn);
                char dueno[8] = "-", copias[16] = "-";
                if (e->es_hogar) {
                    if (e->dueno != NADIE) snprintf(dueno, sizeof dueno, "%u", e->dueno);
                    snprintf(copias, sizeof copias, "0x%08x", e->copias);
                }
                agregar(t, "%-9u 0x%-14llx %-6s %-6u %-6s %-12s %-8u %s\n", e->vpn,
                        (unsigned long long)(g.cfg.base_global + (uint64_t)e->vpn * g.cfg.tam_pagina),
                        NOMBRE_EST[e->estado], e->hogar, dueno, copias, e->fallos, s ? s->nombre : "?");
            }
        pthread_mutex_unlock(&g.franjas[f].mutex);
    }
    agregar(t, "%d paginas\n", lineas);
}

static void consulta_segmentos(texto_t *t)
{
    agregar(t, "Segmentos conocidos por el nodo %u\n", g.yo);
    pthread_mutex_lock(&g.mx_segs);
    for (int i = 0; i < g.n_segs; i++) {
        segmento_t *s = g.segs[i];
        uint64_t base = g.cfg.base_global + (uint64_t)s->vpn_base * g.cfg.tam_pagina;
        agregar(t, "\n'%s' id=0x%x base=0x%llx tam=%llu bytes, %u paginas, creado por el nodo %u (pid %u)\n",
                s->nombre, s->id, (unsigned long long)base, (unsigned long long)s->tam_bytes,
                s->paginas, s->creador, s->pid_creador);
        uint32_t por_nodo[MAX_NODOS] = { 0 };
        for (int k = 0; k < s->n_tramos; k++) por_nodo[s->tramos[k].hogar] += s->tramos[k].cantidad;
        agregar(t, "  paginas por hogar:");
        for (int k = 0; k < MAX_NODOS; k++)
            if (por_nodo[k]) agregar(t, " nodo %d = %u", k, por_nodo[k]);
        agregar(t, "\n  tramos (pagina inicial del segmento - final: hogar):");
        for (int k = 0; k < s->n_tramos && k < 64; k++) {
            uint32_t ini = s->tramos[k].vpn_inicio - s->vpn_base;
            agregar(t, "%s %u-%u:%u", k % 8 ? "" : "\n   ", ini, ini + s->tramos[k].cantidad - 1, s->tramos[k].hogar);
        }
        agregar(t, s->n_tramos > 64 ? "\n   ...\n" : "\n");
    }
    pthread_mutex_unlock(&g.mx_segs);
}

static void consulta_estadisticas(texto_t *t)
{
    estadisticas_t s = g.st;
    agregar(t, "Estadisticas del nodo %u\n", g.yo);
    agregar(t, "  fallos resueltos en el nodo      %llu\n", (unsigned long long)s.fallos_locales);
    agregar(t, "  fallos que necesitaron la red    %llu\n", (unsigned long long)s.fallos_remotos);
    agregar(t, "  latencia media de esos fallos    %.1f us\n",
            s.fallos_remotos ? (double)s.latencia_total_us / s.fallos_remotos : 0.0);
    agregar(t, "  paginas enviadas / recibidas     %llu / %llu\n",
            (unsigned long long)s.paginas_enviadas, (unsigned long long)s.paginas_recibidas);
    agregar(t, "  invalidaciones enviadas / recib. %llu / %llu\n",
            (unsigned long long)s.invalidaciones_enviadas, (unsigned long long)s.invalidaciones_recibidas);
    agregar(t, "  FETCH enviados                   %llu\n", (unsigned long long)s.fetch_enviados);
    agregar(t, "  mensajes enviados / recibidos    %llu / %llu\n",
            (unsigned long long)s.mensajes_enviados, (unsigned long long)s.mensajes_recibidos);
    agregar(t, "  capacidad de hogar libre         %u de %u paginas\n",
            g.nodos[g.yo].libres, g.nodos[g.yo].paginas);
}

static void consulta(int fd, const msg_local_t *m)
{
    texto_t t = { 0 };
    agregar(&t, "%s", "");
    if (!strcmp(m->nombre, "nodos")) consulta_nodos(&t);
    else if (!strcmp(m->nombre, "paginas")) consulta_paginas(&t);
    else if (!strcmp(m->nombre, "segmentos")) consulta_segmentos(&t);
    else if (!strcmp(m->nombre, "estadisticas")) consulta_estadisticas(&t);
    else agregar(&t, "consulta desconocida: %s\n", m->nombre);

    msg_local_t r = { .tipo = LOC_TEXTO, .id = m->id, .valor = t.n };
    enviar_local(fd, &r);
    for (size_t hecho = 0; hecho < t.n; hecho += 32768) {
        size_t n = t.n - hecho < 32768 ? t.n - hecho : 32768;
        if (send(fd, t.s + hecho, n, MSG_NOSIGNAL) < 0) break;
    }
    free(t.s);
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
    } else if (m->tipo == LOC_FALLO || m->tipo == LOC_MALLOC || m->tipo == LOC_BARRERA) {
        mensaje_t *msg = calloc(1, sizeof *msg);
        msg->local = 1;
        msg->fd = fd;
        msg->loc = *m;
        encolar(m->tipo == LOC_BARRERA ? &g.ordenes : &g.locales, msg);
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

/* monitor.c - consultas del sistema (prototipo 2): nodos, paginas, segmentos y
 * estadisticas, de este nodo o de todo el cluster (STATS_REQUEST), en texto para
 * dsmctl y en HTML para el navegador (puerto de datos + 1). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "dsmd.h"

#define MAX_TEXTO_RED 1000000       /* red_leer rechaza cuerpos de mas de 1 MiB */

static const char *NOMBRE_EST[] = { "I", "S", "M", "IS", "IM", "SM", "MI" };

void agregar(texto_t *t, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        if (t->cap - t->n < 512) t->s = realloc(t->s, t->cap = t->cap * 2 + 4096);
        va_start(ap, fmt);
        int k = vsnprintf(t->s + t->n, t->cap - t->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < t->cap - t->n) { t->n += k; return; }
        t->s = realloc(t->s, t->cap = t->cap * 2 + k + 1);
    }
}

static const char *estado_nodo(const nodo_t *n)
{
    return !n->en_linea ? "caido" : n->sospechoso ? "sospechoso" : "en linea";
}

static void consulta_nodos(texto_t *t)
{
    uint64_t ahora = ahora_ms(), total = 0, libre = 0;
    int en_linea = 0;
    agregar(t, "Tabla de nodos (este nodo: %u, coordinador: %u)\n", g.yo, coordinador());
    agregar(t, "%-4s %-11s %-16s %-7s %-8s %-9s %-9s %s\n", "ID", "ESTADO", "IP", "PUERTO", "MONITOR",
            "PAGINAS", "LIBRES", "ULTIMO MENSAJE");
    pthread_mutex_lock(&g.mx_nodos);
    for (int i = 0; i < MAX_NODOS; i++) {
        nodo_t *n = &g.nodos[i];
        if (!n->en_uso) continue;
        char ip[INET_ADDRSTRLEN] = "-", lat[32] = "(este nodo)";
        if (n->ip) inet_ntop(AF_INET, &n->ip, ip, sizeof ip);
        if (i != g.yo) snprintf(lat, sizeof lat, "hace %llu ms", (unsigned long long)(ahora - n->ultimo_ms));
        agregar(t, "%-4d %-11s %-16s %-7u %-8u %-9u %-9u %s\n", i, estado_nodo(n), ip, n->puerto,
                n->puerto + 1, n->paginas, n->libres, lat);
        if (n->en_linea) {
            en_linea++;
            total += (uint64_t)n->paginas * g.cfg.tam_pagina;
            libre += (uint64_t)n->libres * g.cfg.tam_pagina;
        }
    }
    pthread_mutex_unlock(&g.mx_nodos);
    agregar(t, "Nodos en linea: %d | memoria compartida total: %.1f MiB | asignada: %.1f MiB (%.1f%%) | libre: %.1f MiB\n",
            en_linea, total / 1048576.0, (total - libre) / 1048576.0,
            total ? 100.0 * (total - libre) / total : 0.0, libre / 1048576.0);
}

/* Para cada pagina: su segmento y su posicion dentro de el, que es el orden en
 * que la ve el proceso. */
static void consulta_paginas(texto_t *t)
{
    agregar(t, "Tabla de paginas del nodo %u (paginas que este nodo ha usado o de las que es hogar)\n", g.yo);
    agregar(t, "%-9s %-16s %-24s %-6s %-6s %-6s %-12s %s\n", "VPN", "DIRECCION", "SEGMENTO[PAGINA]",
            "ESTADO", "HOGAR", "DUENO", "COPIAS", "FALLOS");
    int lineas = 0;
    for (int f = 0; f < NUM_FRANJAS; f++) {
        pthread_mutex_lock(&g.franjas[f].mutex);
        for (int c = f; c < NUM_CUBETAS; c += NUM_FRANJAS)
            for (entrada_pagina_t *e = g.cubetas[c]; e && lineas < 20000; e = e->sig, lineas++) {
                segmento_t *s = segmento_de_vpn(e->vpn);
                char seg[40] = "(liberado)", dueno[8] = "-", copias[16] = "-";
                if (s) snprintf(seg, sizeof seg, "%.16s[%u]", s->nombre, e->vpn - s->vpn_base);
                if (e->es_hogar) {
                    if (e->dueno != NADIE) snprintf(dueno, sizeof dueno, "%u", e->dueno);
                    snprintf(copias, sizeof copias, "0x%08x", e->copias);
                }
                agregar(t, "%-9u 0x%-14llx %-24s %-6s %-6u %-6s %-12s %u\n", e->vpn,
                        (unsigned long long)(g.cfg.base_global + (uint64_t)e->vpn * g.cfg.tam_pagina),
                        seg, NOMBRE_EST[e->estado], e->hogar, dueno, copias, e->fallos);
            }
        pthread_mutex_unlock(&g.franjas[f].mutex);
    }
    agregar(t, "%d paginas%s\n", lineas, lineas >= 20000 ? " (se muestran las primeras 20000)" : "");
}

static void consulta_segmentos(texto_t *t)
{
    agregar(t, "Segmentos conocidos por el nodo %u\n", g.yo);
    pthread_mutex_lock(&g.mx_segs);
    if (!g.n_segs) agregar(t, "(ninguno)\n");
    for (int i = 0; i < g.n_segs; i++) {
        segmento_t *s = g.segs[i];
        uint64_t base = g.cfg.base_global + (uint64_t)s->vpn_base * g.cfg.tam_pagina;
        agregar(t, "\n'%s' id=0x%x base=0x%llx tam=%llu bytes, %u paginas, creado por el nodo %u (pid %u)\n",
                s->nombre, s->id, (unsigned long long)base, (unsigned long long)s->tam_bytes,
                s->paginas, s->creador, s->pid_creador);
        agregar(t, "  procesos de este nodo que lo usan:");
        int alguno = 0;
        for (int k = 0; k < MAX_PIDS_SEG; k++)
            if (s->pids[k]) { agregar(t, " %u", s->pids[k]); alguno = 1; }
        agregar(t, alguno ? "\n" : " ninguno\n");
        uint32_t por_nodo[MAX_NODOS] = { 0 };
        for (int k = 0; k < s->n_tramos; k++) por_nodo[s->tramos[k].hogar] += s->tramos[k].cantidad;
        agregar(t, "  paginas por hogar:");
        for (int k = 0; k < MAX_NODOS; k++)
            if (por_nodo[k]) agregar(t, " nodo %d = %u", k, por_nodo[k]);
        agregar(t, "\n  tramos en orden (pagina inicial del segmento - final: hogar):");
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
    uint32_t limite = g.cfg.cache_remota / g.cfg.tam_pagina;
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
    agregar(t, "  copias de paginas ajenas         %u (limite %u, reemplazo %s)\n", cache_ocupadas(),
            limite, g.cfg.reemplazo == REEMPLAZO_FIFO ? "FIFO" : "reloj");
    agregar(t, "  desalojos en S / en M            %llu / %llu\n",
            (unsigned long long)s.desalojos_s, (unsigned long long)s.desalojos_m);
    agregar(t, "  WRITEBACK recibidos como hogar   %llu\n", (unsigned long long)s.writebacks_recibidos);
    agregar(t, "  segundas oportunidades (reloj)   %llu\n", (unsigned long long)s.segundas_oportunidades);
    int procs = 0;
    pthread_mutex_lock(&g.mx_procs);
    for (int i = 0; i < MAX_PROCS; i++) procs += g.procs[i].pid != 0;
    pthread_mutex_unlock(&g.mx_procs);
    agregar(t, "  procesos conectados              %d\n", procs);
}

static void consulta_local(const char *nombre, texto_t *t)
{
    if (!strcmp(nombre, "nodos")) consulta_nodos(t);
    else if (!strcmp(nombre, "paginas")) consulta_paginas(t);
    else if (!strcmp(nombre, "segmentos")) consulta_segmentos(t);
    else if (!strcmp(nombre, "estadisticas")) consulta_estadisticas(t);
    else agregar(t, "consulta desconocida: %s\n", nombre);
}

/* Global: la parte de este nodo se arma aqui y la de cada otro nodo se le pide
 * con STATS_REQUEST. Se llama desde hilos que pueden esperar a la red (un hilo de
 * consulta o el del monitor), nunca desde el hilo de ordenes. */
void consulta_texto(const char *nombre, int global, texto_t *t)
{
    if (!global || !strcmp(nombre, "nodos")) { consulta_local(nombre, t); return; }
    consulta_nodos(t);
    for (int i = 0; i < MAX_NODOS; i++) {
        if (!g.nodos[i].en_linea) continue;
        agregar(t, "\n==================== nodo %d ====================\n", i);
        if (i == g.yo) { consulta_local(nombre, t); continue; }
        msg_stats_request_t q = { { 0 } };
        snprintf(q.consulta, sizeof q.consulta, "%s", nombre);
        uint32_t seq = pend_nueva(1);
        int r = red_enviar(i, MSG_STATS_REQUEST, 0, seq, &q, sizeof q, NULL);
        if (r) pend_completar(seq, r, NULL);
        mensaje_t *resp = NULL;
        r = pend_esperar(seq, &resp);
        if (r || !resp) agregar(t, "(sin respuesta: %s)\n", dsm_strerror(r));
        else agregar(t, "%.*s", (int)resp->len, (char *)resp->cuerpo);
        mensaje_liberar(resp);
    }
}

/* Se atiende en el hilo de ordenes: solo lee tablas, no espera a nadie */
void atender_stats_request(mensaje_t *m)
{
    texto_t t = { 0 };
    agregar(&t, "%s", "");
    if (m->len >= sizeof(msg_stats_request_t)) {
        msg_stats_request_t *q = (msg_stats_request_t *)m->cuerpo;
        q->consulta[DSM_MAX_NOMBRE - 1] = 0;
        consulta_local(q->consulta, &t);
    }
    if (t.n > MAX_TEXTO_RED) {
        t.n = MAX_TEXTO_RED - 64;
        agregar(&t, "\n... (respuesta recortada)\n");
    }
    red_enviar(m->cab.origen, MSG_STATS_REPLY, BANDERA_RESP, m->cab.secuencia, t.s, t.n, NULL);
    free(t.s);
    mensaje_liberar(m);
}

/* ---------- monitor web ----------
 * Cada nodo escucha en su puerto de datos + 1, asi que varios nodos en la misma
 * computadora se ven por separado. "/" muestra este nodo y "/global" todo el
 * sistema. La pagina se recarga sola cada 2 segundos. */
static void seccion(texto_t *h, const char *titulo, const texto_t *t)
{
    agregar(h, "<h2>%s</h2><pre>", titulo);
    for (size_t i = 0; i < t->n; i++) {
        char c = t->s[i];
        if (c == '<') agregar(h, "&lt;");
        else if (c == '>') agregar(h, "&gt;");
        else if (c == '&') agregar(h, "&amp;");
        else agregar(h, "%c", c);
    }
    agregar(h, "</pre>");
}

static void atender_http(int fd)
{
    char req[512] = { 0 };
    if (recv(fd, req, sizeof req - 1, 0) <= 0) return;
    int global = !strncmp(req, "GET /global", 11);
    texto_t h = { 0 };
    agregar(&h, "<!doctype html><html lang='es'><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='2'><title>DSM nodo %u</title><style>"
                "body{font-family:system-ui,sans-serif;margin:24px;background:#f7f8fa;color:#1d2633}"
                "h1{color:#1f4e9c}h2{font-size:17px;margin-top:22px}"
                "pre{background:#fff;border:1px solid #d5dbe3;border-radius:6px;padding:12px;"
                "overflow-x:auto;font-size:12.5px}a{margin-right:18px}</style></head><body>"
                "<h1>DSM &middot; nodo %u%s</h1><p><a href='/'>Este nodo</a><a href='/global'>Todo el sistema</a>"
                "Se actualiza cada 2 s.</p>", g.yo, g.yo, global ? " &middot; vista global" : "");
    const char *nombres[] = { "nodos", "estadisticas", "segmentos", "paginas" };
    const char *titulos[] = { "Nodos", "Estad&iacute;sticas", "Segmentos y procesos", "Tabla de p&aacute;ginas" };
    for (int i = 0; i < 4; i++) {
        texto_t t = { 0 };
        agregar(&t, "%s", "");
        consulta_texto(nombres[i], global, &t);
        seccion(&h, titulos[i], &t);
        free(t.s);
    }
    agregar(&h, "</body></html>");
    char cab[160];
    int k = snprintf(cab, sizeof cab, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                      "Content-Length: %zu\r\nConnection: close\r\n\r\n", h.n);
    if (send(fd, cab, k, MSG_NOSIGNAL) == k)
        for (size_t hecho = 0; hecho < h.n;) {
            ssize_t w = send(fd, h.s + hecho, h.n - hecho, MSG_NOSIGNAL);
            if (w <= 0) break;
            hecho += w;
        }
    free(h.s);
}

void *hilo_monitor(void *arg)
{
    (void)arg;
    uint16_t puerto = g.cfg.puerto_minimo + 2 * g.yo + 1;
    int ls = socket(AF_INET, SOCK_STREAM, 0), uno = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(puerto),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) {
        LOG("no se pudo abrir el monitor web en el puerto %u", puerto);
        close(ls);
        return NULL;
    }
    LOG("monitor web en http://localhost:%u/", puerto);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        struct timeval to = { .tv_sec = 2 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
        atender_http(fd);
        close(fd);
    }
    return NULL;
}

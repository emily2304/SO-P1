/* dsm.c - memfd con la memoria del nodo, tabla de paginas, busqueda de segmentos
 * y cache de copias ajenas con desalojo FIFO o reloj (prototipo 2) */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dsmd.h"

int memoria_iniciar(void)
{
    g.paginas_ventana = g.cfg.ventana_por_nodo / g.cfg.tam_pagina;
    g.tam_espacio = (uint64_t)MAX_NODOS * g.cfg.ventana_por_nodo;
    g.sig_vpn = g.yo * g.paginas_ventana;

    /* el memfd es disperso: solo ocupan RAM las paginas que se escriben */
    g.memfd = memfd_create("dsm", 0);
    if (g.memfd < 0 || ftruncate(g.memfd, g.tam_espacio) < 0) return DSM_ENOMEM;
    g.vista = mmap(NULL, g.tam_espacio, PROT_READ | PROT_WRITE, MAP_SHARED, g.memfd, 0);
    if (g.vista == MAP_FAILED) return DSM_ENOMEM;

    /* la cache de copias ajenas guarda vpns; crece si hace falta */
    g.cache.cap = g.cfg.cache_remota / g.cfg.tam_pagina + 64;
    g.cache.vpn = calloc(g.cache.cap, sizeof(uint32_t));
    pthread_mutex_init(&g.cache.mutex, NULL);

    for (int i = 0; i < NUM_FRANJAS; i++) {
        pthread_mutex_init(&g.franjas[i].mutex, NULL);
        pthread_cond_init(&g.franjas[i].cambio, NULL);
    }
    return DSM_OK;
}

franja_t *franja_de(uint32_t vpn) { return &g.franjas[vpn % NUM_FRANJAS]; }

uint8_t *dir_pagina(uint32_t vpn) { return g.vista + (uint64_t)vpn * g.cfg.tam_pagina; }

void pagina_liberar_memoria(uint32_t vpn)
{
    fallocate(g.memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
              (off_t)vpn * g.cfg.tam_pagina, g.cfg.tam_pagina);
}

segmento_t *segmento_de_vpn(uint32_t vpn)
{
    segmento_t *r = NULL;
    pthread_mutex_lock(&g.mx_segs);
    for (int i = 0; i < g.n_segs && !r; i++)
        if (vpn >= g.segs[i]->vpn_base && vpn < g.segs[i]->vpn_base + g.segs[i]->paginas)
            r = g.segs[i];
    pthread_mutex_unlock(&g.mx_segs);
    return r;
}

segmento_t *segmento_por_nombre(const char *nombre)
{
    segmento_t *r = NULL;
    pthread_mutex_lock(&g.mx_segs);
    for (int i = 0; i < g.n_segs && !r; i++)
        if (!strncmp(g.segs[i]->nombre, nombre, DSM_MAX_NOMBRE)) r = g.segs[i];
    pthread_mutex_unlock(&g.mx_segs);
    return r;
}

/* Busca la entrada de una pagina y la crea la primera vez que se usa.
 * Se llama con la franja de la pagina tomada. */
entrada_pagina_t *pagina_obtener(uint32_t vpn)
{
    entrada_pagina_t **cub = &g.cubetas[vpn % NUM_CUBETAS];
    for (entrada_pagina_t *e = *cub; e; e = e->sig)
        if (e->vpn == vpn) return e;

    segmento_t *s = segmento_de_vpn(vpn);
    if (!s) return NULL;
    uint16_t hogar = NADIE;
    for (int i = 0; i < s->n_tramos; i++)
        if (vpn >= s->tramos[i].vpn_inicio && vpn < s->tramos[i].vpn_inicio + s->tramos[i].cantidad)
            hogar = s->tramos[i].hogar;
    if (hogar == NADIE) return NULL;

    /* una pagina nueva empieza en ceros, en M en su hogar y en I en los demas */
    entrada_pagina_t *e = calloc(1, sizeof *e);
    e->vpn = vpn;
    e->hogar = hogar;
    e->es_hogar = (hogar == g.yo);
    e->estado = e->es_hogar ? EST_M : EST_I;
    e->dueno = e->es_hogar ? g.yo : NADIE;
    e->id_segmento = s->id;
    e->sig = *cub;
    *cub = e;
    return e;
}

/* Busca la entrada de una pagina sin crearla. Con la franja tomada. */
entrada_pagina_t *pagina_buscar(uint32_t vpn)
{
    for (entrada_pagina_t *e = g.cubetas[vpn % NUM_CUBETAS]; e; e = e->sig)
        if (e->vpn == vpn) return e;
    return NULL;
}

/* dsm_free: quita las entradas de un rango. No se borra una pagina con una
 * transaccion en curso (otro hilo tiene su puntero); queda en I y sin segmento. */
void pagina_quitar_rango(uint32_t vpn, uint32_t n)
{
    for (uint32_t v = vpn; v < vpn + n; v++) {
        franja_t *f = franja_de(v);
        pthread_mutex_lock(&f->mutex);
        for (entrada_pagina_t **pp = &g.cubetas[v % NUM_CUBETAS]; *pp; pp = &(*pp)->sig) {
            entrada_pagina_t *e = *pp;
            if (e->vpn != v) continue;
            if (e->ocupada || e->entregando || e->estado >= EST_IS) { e->estado = EST_I; break; }
            *pp = e->sig;
            free(e);
            break;
        }
        pthread_cond_broadcast(&f->cambio);
        pthread_mutex_unlock(&f->mutex);
    }
    fallocate(g.memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
              (off_t)vpn * g.cfg.tam_pagina, (off_t)n * g.cfg.tam_pagina);
}

/* ---------- cache de copias ajenas ----------
 * Orden de candados: franja -> cache. Nunca se toma una franja con la cache tomada. */

static void cache_empujar(uint32_t vpn)
{
    cache_t *c = &g.cache;
    if (c->n == c->cap) {                       /* crecer y dejar el arreglo en orden */
        uint32_t *nuevo = calloc(c->cap * 2, sizeof(uint32_t));
        for (uint32_t i = 0; i < c->n; i++) nuevo[i] = c->vpn[(c->cabeza + i) % c->cap];
        free(c->vpn);
        c->vpn = nuevo;
        c->cap *= 2;
        c->cabeza = 0;
    }
    c->vpn[(c->cabeza + c->n) % c->cap] = vpn;
    c->n++;
}

/* Una pagina de otro hogar paso a S o M: entra al final de la cola. Con la franja tomada. */
void cache_registrar(entrada_pagina_t *e)
{
    if (e->es_hogar || e->en_cache) return;
    e->en_cache = 1;
    e->referida = 1;
    pthread_mutex_lock(&g.cache.mutex);
    cache_empujar(e->vpn);
    pthread_mutex_unlock(&g.cache.mutex);
}

/* Un proceso local uso la pagina: el reloj le dara una segunda oportunidad */
void pagina_marcar_usada(entrada_pagina_t *e) { e->referida = 1; }

uint32_t cache_ocupadas(void)
{
    pthread_mutex_lock(&g.cache.mutex);
    uint32_t n = g.cache.n;
    pthread_mutex_unlock(&g.cache.mutex);
    return n;
}

static int cache_sacar(uint32_t *vpn)
{
    pthread_mutex_lock(&g.cache.mutex);
    int hay = g.cache.n > 0;
    if (hay) {
        *vpn = g.cache.vpn[g.cache.cabeza];
        g.cache.cabeza = (g.cache.cabeza + 1) % g.cache.cap;
        g.cache.n--;
    }
    pthread_mutex_unlock(&g.cache.mutex);
    return hay;
}

/* Elige la pagina a desalojar. FIFO: la mas vieja. Reloj: la mas vieja que no
 * se haya usado desde la ultima pasada; a las usadas se les borra el bit y
 * vuelven al final (segunda oportunidad). Las entradas viejas (paginas que ya
 * se invalidaron) se descartan por el camino. */
static int elegir_victima(uint32_t excepto, uint32_t *victima)
{
    uint32_t vueltas = 2 * cache_ocupadas() + 2;
    uint32_t v;
    while (vueltas-- > 0 && cache_sacar(&v)) {
        franja_t *f = franja_de(v);
        pthread_mutex_lock(&f->mutex);
        entrada_pagina_t *e = pagina_buscar(v);
        int valida = e && !e->es_hogar && !e->entregando && (e->estado == EST_S || e->estado == EST_M);
        if (!valida) {                          /* ya no es una copia: se olvida */
            if (e && e->estado < EST_IS && !e->entregando) e->en_cache = 0;
            else if (e) { pthread_mutex_lock(&g.cache.mutex); cache_empujar(v); pthread_mutex_unlock(&g.cache.mutex); }
            pthread_mutex_unlock(&f->mutex);
            continue;
        }
        int segunda = g.cfg.reemplazo == REEMPLAZO_RELOJ && e->referida;
        if (v == excepto || segunda) {
            if (segunda) { e->referida = 0; SUMAR(segundas_oportunidades, 1); }
            pthread_mutex_lock(&g.cache.mutex);
            cache_empujar(v);
            pthread_mutex_unlock(&g.cache.mutex);
            pthread_mutex_unlock(&f->mutex);
            continue;
        }
        e->en_cache = 0;
        pthread_mutex_unlock(&f->mutex);
        *victima = v;
        return 1;
    }
    return 0;
}

/* Mientras haya mas copias ajenas que las que permite cache_remota, desaloja.
 * La llaman los hilos locales despues de traer una pagina; pueden esperar al
 * hogar (WRITEBACK), igual que cuando piden una pagina. */
void cache_ajustar(uint32_t excepto)
{
    uint32_t limite = g.cfg.cache_remota / g.cfg.tam_pagina;
    uint32_t v;
    while (cache_ocupadas() > limite && elegir_victima(excepto, &v))
        devolver_pagina(v);
}

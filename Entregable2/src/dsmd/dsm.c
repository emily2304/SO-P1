/* dsm.c - memfd con la memoria del nodo, tabla de paginas y busqueda de segmentos */
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

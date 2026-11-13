/* config.c - lectura de dsm.conf (secciones [x], lineas clave = valor, # comentarios) */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "dsm.h"

static uint64_t tam_con_sufijo(const char *v)
{
    char *fin;
    uint64_t n = strtoull(v, &fin, 0);
    switch (toupper((unsigned char)*fin)) {
    case 'K': n <<= 10; break;
    case 'M': n <<= 20; break;
    case 'G': n <<= 30; break;
    }
    return n;
}

static char *recortar(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *f = s + strlen(s);
    while (f > s && isspace((unsigned char)f[-1])) *--f = 0;
    return s;
}

static void valores_por_defecto(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->tam_pagina = 4096;
    c->memoria_por_nodo = 64ull << 20;
    c->base_global = 0x600000000000ull;
    c->ventana_por_nodo = 4ull << 30;
    c->bloque_reparto = 16;
    c->politica_defecto = DSM_REPARTIDA;
    c->puerto_minimo = 27500;
    c->puerto_maximo = 27599;
    strcpy(c->socket_local, "/tmp/dsm-27500-%d.sock");
    c->hilos_trabajadores = 4;
    c->tam_cola = 1024;
    c->intervalo_latido = 500;
    c->caida_tras = 3000;
    c->plazo_peticion = 2000;
}

int config_leer(const char *ruta, config_t *c)
{
    FILE *f = fopen(ruta, "r");
    if (!f) return DSM_ECONF;
    valores_por_defecto(c);

    uint32_t h = 2166136261u;
    char linea[256];
    while (fgets(linea, sizeof linea, f)) {
        char *p = strchr(linea, '#');
        if (p) *p = 0;
        /* el hash se calcula sin espacios ni comentarios */
        for (p = linea; *p; p++)
            if (!isspace((unsigned char)*p)) { h ^= (uint8_t)*p; h *= 16777619u; }

        char *s = recortar(linea);
        if (!*s || *s == '[') continue;
        char *igual = strchr(s, '=');
        if (!igual) { fclose(f); return DSM_ECONF; }
        *igual = 0;
        char *k = recortar(s), *v = recortar(igual + 1);

        if      (!strcmp(k, "tam_pagina"))         c->tam_pagina = tam_con_sufijo(v);
        else if (!strcmp(k, "memoria_por_nodo"))   c->memoria_por_nodo = tam_con_sufijo(v);
        else if (!strcmp(k, "base_global"))        c->base_global = strtoull(v, NULL, 0);
        else if (!strcmp(k, "ventana_por_nodo"))   c->ventana_por_nodo = tam_con_sufijo(v);
        else if (!strcmp(k, "bloque_reparto"))     c->bloque_reparto = atoi(v);
        else if (!strcmp(k, "politica_defecto"))   c->politica_defecto = !strcmp(v, "repartida");
        else if (!strcmp(k, "puerto_minimo"))      c->puerto_minimo = atoi(v);
        else if (!strcmp(k, "puerto_maximo"))      c->puerto_maximo = atoi(v);
        else if (!strcmp(k, "socket_local"))       snprintf(c->socket_local, sizeof c->socket_local, "%s", v);
        else if (!strcmp(k, "hilos_trabajadores")) c->hilos_trabajadores = atoi(v);
        else if (!strcmp(k, "tam_cola_mensajes"))  c->tam_cola = atoi(v);
        else if (!strcmp(k, "intervalo_latido"))   c->intervalo_latido = atoi(v);
        else if (!strcmp(k, "caida_tras"))         c->caida_tras = atoi(v);
        else if (!strcmp(k, "plazo_peticion"))     c->plazo_peticion = atoi(v);
        else if (!strcmp(k, "semilla") && c->n_semillas < MAX_SEMILLAS)
            snprintf(c->semilla[c->n_semillas++], 64, "%s", v);
    }
    fclose(f);
    c->hash = h;

    if (c->tam_pagina < 4096 || c->tam_pagina % 4096 || c->memoria_por_nodo < (1u << 20) ||
        c->puerto_maximo < c->puerto_minimo + 2 * 32 || c->hilos_trabajadores < 1 || c->tam_cola < 16)
        return DSM_ECONF;
    return DSM_OK;
}

void config_ruta_socket(const config_t *c, int nodo, char *dst, int tam)
{
    snprintf(dst, tam, c->socket_local, nodo);
}

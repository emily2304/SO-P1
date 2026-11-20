#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_SEMILLAS 8

enum { REEMPLAZO_FIFO = 0, REEMPLAZO_RELOJ = 1 };

typedef struct {
    uint32_t tam_pagina;
    uint64_t memoria_por_nodo;
    uint64_t cache_remota;      /* bytes de copias de paginas ajenas antes de desalojar */
    int      reemplazo;         /* REEMPLAZO_FIFO o REEMPLAZO_RELOJ */
    uint64_t base_global;
    uint64_t ventana_por_nodo;
    uint32_t bloque_reparto;
    int      politica_defecto;
    uint16_t puerto_minimo, puerto_maximo;
    char     semilla[MAX_SEMILLAS][64];
    int      n_semillas;
    char     socket_local[100];
    int      hilos_trabajadores;
    int      tam_cola;
    int      intervalo_latido;
    int      sospecha_tras;
    int      caida_tras;
    int      plazo_peticion;
    uint32_t hash;              /* FNV-1a del archivo normalizado */
} config_t;

int  config_leer(const char *ruta, config_t *c);
void config_ruta_socket(const config_t *c, int nodo, char *dst, int tam);

#endif

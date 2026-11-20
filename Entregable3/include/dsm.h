/* dsm.h - API publica del sistema DSM (prototipo 2) */
#ifndef DSM_H
#define DSM_H

#include <stddef.h>
#include <stdint.h>

#define DSM_MAX_NOMBRE 32

typedef enum {
    DSM_OK          =   0,
    DSM_ECONF       =  -1,  /* configuracion ausente o invalida */
    DSM_ENODEMONIO  =  -2,  /* no hay un dsmd en este nodo */
    DSM_ENOMEM      =  -3,  /* no alcanza la memoria del cluster */
    DSM_EINVAL      =  -4,
    DSM_ENOENT      =  -5,  /* segmento inexistente */
    DSM_EEXISTE     =  -6,  /* ya hay un segmento con ese nombre */
    DSM_ETIEMPO     =  -7,  /* se vencio el plazo */
    DSM_ERED        =  -8,
    DSM_ENODOCAIDO  =  -9,  /* el nodo que se necesitaba no responde */
    DSM_EPROTOCOLO  = -10,
    DSM_ENOINICIADO = -11
} dsm_error_t;

typedef enum { DSM_LOCAL_PRIMERO = 0, DSM_REPARTIDA = 1 } dsm_politica_t;

typedef struct {
    uint16_t id_nodo;
    uint16_t nodos_en_linea;
    uint32_t tam_pagina;
    uint64_t memoria_total;   /* bytes que aportan los nodos en linea */
    uint64_t memoria_libre;
} dsm_info_t;

int         dsm_iniciar(const char *conf);
void        dsm_terminar(void);
void       *dsm_malloc(size_t tam, const char *nombre, dsm_politica_t pol);
void       *dsm_buscar(const char *nombre, size_t *tam);
int         dsm_free(void *ptr);
int         dsm_barrera(const char *nombre, int n);
int         dsm_candado(const char *nombre);
int         dsm_soltar(const char *nombre);
int         dsm_info(dsm_info_t *info);
int         dsm_ultimo_error(void);
const char *dsm_strerror(int codigo);

#endif

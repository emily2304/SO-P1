/* libdsm.h - estado interno compartido por api.c y pager.c */
#ifndef LIBDSM_H
#define LIBDSM_H

#include <stdint.h>
#include "protocolo.h"

extern int      dsm_sock;           /* socket Unix con el dsmd local */
extern uint8_t *dsm_base;           /* vista de la aplicacion en la base fija */
extern uint64_t dsm_tam_espacio;
extern uint32_t dsm_tam_pagina;

int  pager_iniciar(void);
void pager_terminar(void);
void api_entregar_respuesta(const msg_local_t *m);

#endif

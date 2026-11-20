/* dsmd.h - estructuras internas del demonio */
#ifndef DSMD_H
#define DSMD_H

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include "config.h"
#include "protocolo.h"

#define MAX_PROCS      64
#define MAX_SEGMENTOS  256
#define MAX_PEND       1024
#define NUM_FRANJAS    1024
#define NUM_CUBETAS    65536
#define MAX_BARRERAS   32
#define MAX_CANDADOS   64
#define MAX_PIDS_SEG   16

#define LOG(...) do { fprintf(stderr, "[dsmd %u] ", (unsigned)g.yo); \
                      fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

/* Mensaje ya recibido, en orden del host */
typedef struct {
    cabecera_t  cab;
    int         fd;         /* conexion o socket de la app de donde llego */
    int         local;      /* 1 si viene de una aplicacion del nodo */
    msg_local_t loc;
    uint32_t    len;
    uint8_t    *cuerpo;
} mensaje_t;

typedef struct {
    mensaje_t     **items;
    int             tam, cabeza, cola;
    sem_t           huecos, llenos;
    pthread_mutex_t mutex;
} cola_t;

typedef struct {
    int             en_uso, en_linea;
    int             sospechoso;     /* sin mensajes por mas de sospecha_tras */
    uint32_t        ip;             /* orden de red */
    uint16_t        puerto;
    uint32_t        paginas, libres;
    int             fd;
    pthread_mutex_t envio;
    uint64_t        ultimo_ms;
} nodo_t;

typedef struct entrada_pagina {
    uint32_t vpn;
    uint8_t  estado;        /* EST_I, EST_S, EST_M o transitorio */
    uint8_t  es_hogar;
    uint16_t hogar;
    uint16_t dueno;         /* solo en el hogar: quien la tiene en M */
    uint8_t  ocupada;       /* solo en el hogar: atendiendo una peticion */
    uint8_t  reservado;
    uint32_t copias;        /* solo en el hogar: mapa de bits de nodos en S */
    uint32_t version;
    uint32_t id_segmento;
    uint32_t fallos;
    uint8_t  en_cache;      /* copia ajena registrada en la cache de desalojo */
    uint8_t  referida;      /* bit de referencia del algoritmo del reloj */
    uint8_t  entregando;    /* un FETCH esta enviando esta pagina: no borrarla ni concederla */
    uint8_t  reservado2;
    struct entrada_pagina *sig;
} entrada_pagina_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cambio;     /* se avisa cada vez que una pagina cambia de estado */
} franja_t;

typedef struct {
    uint32_t id, vpn_base, paginas;
    uint64_t tam_bytes;
    uint16_t creador, n_tramos;
    uint32_t pid_creador;
    char     nombre[DSM_MAX_NOMBRE];
    tramo_t *tramos;
    uint32_t pids[MAX_PIDS_SEG];    /* procesos de ESTE nodo que lo crearon o buscaron */
} segmento_t;

/* Copias de paginas cuyo hogar es otro nodo, en el orden en que llegaron.
 * FIFO desaloja la mas vieja; el reloj le da una segunda oportunidad a las
 * que se usaron desde la ultima vuelta de la manecilla. */
typedef struct {
    uint32_t       *vpn;
    uint32_t        cap, n, cabeza;     /* arreglo circular: cabeza = la mas vieja */
    pthread_mutex_t mutex;
} cache_t;

typedef struct {
    int      activo;
    uint32_t seq;
    int      faltan;        /* respuestas que todavia se esperan */
    int      error;
    mensaje_t *resp;
} pendiente_t;

#define MAX_DEBE 32

typedef struct {
    int      fd;
    uint32_t pid;
    uint32_t debe[MAX_DEBE];    /* revocaciones que este proceso aun no confirma */
    int      n_debe;
} proc_t;

typedef struct {
    uint64_t fallos_locales, fallos_remotos, paginas_enviadas, paginas_recibidas;
    uint64_t invalidaciones_enviadas, invalidaciones_recibidas, fetch_enviados;
    uint64_t mensajes_enviados, mensajes_recibidos, latencia_total_us;
    uint64_t desalojos_s, desalojos_m, writebacks_recibidos, segundas_oportunidades;
} estadisticas_t;

typedef struct {
    config_t        cfg;
    uint16_t        yo;
    char            ruta_socket[108];

    nodo_t          nodos[MAX_NODOS];
    pthread_mutex_t mx_nodos;

    int             memfd;
    uint8_t        *vista;          /* vista del sistema, siempre lectura y escritura */
    uint64_t        tam_espacio;
    uint32_t        paginas_ventana;

    segmento_t     *segs[MAX_SEGMENTOS];
    int             n_segs;
    uint32_t        sig_vpn;        /* siguiente pagina libre en la ventana propia */
    pthread_mutex_t mx_segs;

    cache_t         cache;          /* copias ajenas, para el desalojo */

    franja_t        franjas[NUM_FRANJAS];
    entrada_pagina_t *cubetas[NUM_CUBETAS];

    cola_t          locales;        /* fallos y malloc de las apps: esperan a un hogar */
    cola_t          peticiones;     /* PAGE_REQUEST de otros nodos: esperan solo ordenes */
    cola_t          ordenes;        /* mensajes que se atienden sin esperar a nadie */

    pendiente_t     pend[MAX_PEND];
    pthread_mutex_t mx_pend;
    pthread_cond_t  cv_pend;
    uint32_t        sig_seq;

    proc_t          procs[MAX_PROCS];
    pthread_mutex_t mx_procs;

    estadisticas_t  st;
    int             fd_escucha;
    int             tubo[2];        /* despierta al escuchador cuando hay conexiones nuevas */
} dsmd_t;

extern dsmd_t g;

#define SUMAR(campo, n) __atomic_add_fetch(&g.st.campo, (n), __ATOMIC_RELAXED)

/* cola.c */
void       cola_iniciar(cola_t *c, int tam);
void       encolar(cola_t *c, mensaje_t *m);
mensaje_t *desencolar(cola_t *c);
uint32_t   pend_nueva(int faltan);
void       pend_completar(uint32_t seq, int error, mensaje_t *resp);
int        pend_esperar(uint32_t seq, mensaje_t **resp);
void       mensaje_liberar(mensaje_t *m);
uint64_t   ahora_ms(void);
uint64_t   ahora_us(void);

/* network.c */
int        red_escuchar(uint16_t puerto);
int        red_conectar(uint32_t ip, uint16_t puerto);
int        red_enviar(uint16_t dest, uint16_t tipo, uint8_t banderas, uint32_t seq,
                      const void *cuerpo, uint32_t len, const void *pagina);
int        red_enviar_fd(int fd, uint16_t tipo, uint8_t banderas, uint32_t seq,
                         const void *cuerpo, uint32_t len, const void *pagina);
mensaje_t *red_leer(int fd);
void       red_responder_error(const mensaje_t *m, int codigo);
void       red_despertar(void);
void      *hilo_escuchador(void *arg);

/* membresia.c */
int        membresia_fundar(void);
int        membresia_unirse(void);
void       atender_join(mensaje_t *m);
void       atender_connect(mensaje_t *m);
void       atender_announce(mensaje_t *m);
void       atender_heartbeat(mensaje_t *m);
void       nodo_caido(uint16_t id);
uint16_t   coordinador(void);
void      *hilo_latidos(void *arg);

/* dsm.c */
void       cache_registrar(entrada_pagina_t *e);
void       cache_ajustar(uint32_t excepto);
uint32_t   cache_ocupadas(void);
void       pagina_marcar_usada(entrada_pagina_t *e);
int        memoria_iniciar(void);
franja_t  *franja_de(uint32_t vpn);
entrada_pagina_t *pagina_obtener(uint32_t vpn);
entrada_pagina_t *pagina_buscar(uint32_t vpn);
void       pagina_quitar_rango(uint32_t vpn, uint32_t n);
uint8_t   *dir_pagina(uint32_t vpn);
void       pagina_liberar_memoria(uint32_t vpn);
segmento_t *segmento_de_vpn(uint32_t vpn);
segmento_t *segmento_por_nombre(const char *nombre);

/* directorio.c */
void       resolver_fallo(mensaje_t *m);
void       atender_page_request(mensaje_t *m);
void       atender_page_data(mensaje_t *m);
void       atender_invalidate(mensaje_t *m);
void       atender_fetch(mensaje_t *m);
void       atender_writeback(mensaje_t *m);
int        devolver_pagina(uint32_t vpn);

/* asignacion.c */
void       atender_malloc(mensaje_t *m);
void       atender_segment_create(mensaje_t *m);
void       enviar_segmentos_a(uint16_t id);
void       atender_free(mensaje_t *m);
void       atender_segment_free(mensaje_t *m);
void       segmento_registrar_pid(segmento_t *s, uint32_t pid);

/* sincro.c */
void       atender_barrera_local(mensaje_t *m);
void       atender_barrier_enter(mensaje_t *m);
void       atender_barrier_release(mensaje_t *m);
void       atender_candado_local(mensaje_t *m);
void       atender_soltar_local(mensaje_t *m);
void       atender_lock_request(mensaje_t *m);
void       atender_lock_grant(mensaje_t *m);
void       atender_lock_release(mensaje_t *m);
void       sincro_nodo_caido(uint16_t id);

/* clientes.c */
void      *hilo_clientes(void *arg);
void       responder_app(int fd, uint32_t id, int error, uint64_t valor, uint64_t valor2);
void       conceder(int fd, uint32_t ranura, uint32_t vpn, int prot, int error);
int        revocar_locales(uint32_t vpn, int prot);
int        revocar_rango(uint32_t vpn, uint32_t paginas);

/* monitor.c */
typedef struct { char *s; size_t n, cap; } texto_t;
void       agregar(texto_t *t, const char *fmt, ...);
void       consulta_texto(const char *nombre, int global, texto_t *t);
void       atender_stats_request(mensaje_t *m);
void      *hilo_monitor(void *arg);

#endif

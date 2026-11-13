/* protocolo.h - mensajes entre nodos (TCP) y entre libdsm y dsmd (socket Unix) */
#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdint.h>
#include "dsm.h"

#define DSM_MAGICO     0x44534D31u   /* "DSM1" */
#define DSM_VERSION    1
#define NODO_TODOS     0xFFFF
#define NADIE          0xFFFF
#define MAX_NODOS      32
#define BANDERA_RESP   0x01
#define EMPACADO       __attribute__((packed))

enum {
    MSG_JOIN            = 0x0101,
    MSG_JOIN_ACK        = 0x0102,
    MSG_NODE_ANNOUNCE   = 0x0103,
    MSG_HEARTBEAT       = 0x0104,
    MSG_NODE_DOWN       = 0x0106,
    MSG_CONNECT         = 0x0107,   /* nuevo en el prototipo 1 */
    MSG_PAGE_REQUEST    = 0x0201,
    MSG_PAGE_DATA       = 0x0202,
    MSG_INVALIDATE      = 0x0203,
    MSG_INVALIDATE_ACK  = 0x0204,
    MSG_FETCH           = 0x0205,
    MSG_FETCH_REPLY     = 0x0206,
    MSG_SEGMENT_CREATE  = 0x0303,
    MSG_SEGMENT_ACK     = 0x0304,
    MSG_BARRIER_ENTER   = 0x0401,
    MSG_BARRIER_RELEASE = 0x0402,
    MSG_ERROR           = 0x0F01
};

/* Estados MSI de una pagina en un nodo, con los tres transitorios */
enum { EST_I = 0, EST_S, EST_M, EST_IS, EST_IM, EST_SM };
enum { ACCESO_LECTURA = 0, ACCESO_ESCRITURA = 1 };

typedef struct EMPACADO {
    uint32_t magico;
    uint8_t  version;
    uint8_t  banderas;
    uint16_t tipo;
    uint16_t origen;
    uint16_t destino;
    uint32_t secuencia;
    uint32_t longitud;
    uint32_t reservado;
} cabecera_t;

typedef struct EMPACADO {
    uint32_t ip;
    uint16_t puerto_datos;
    uint16_t puerto_monitor;
    uint32_t paginas;           /* capacidad que aporta como hogar */
    uint32_t hash_conf;
    char     nombre[DSM_MAX_NOMBRE];
    uint32_t reservado;
} msg_join_t;

typedef struct EMPACADO {
    uint16_t id;
    uint8_t  estado;            /* 0 en linea, 2 caido */
    uint8_t  reservado;
    uint32_t ip;
    uint16_t puerto_datos;
    uint16_t puerto_monitor;
    uint32_t paginas;
    uint32_t libres;
    uint32_t reservado2;
} entrada_nodo_t;

typedef struct EMPACADO {
    uint16_t id_asignado;
    uint16_t cantidad;          /* le siguen 'cantidad' entrada_nodo_t */
    uint32_t reservado;
} msg_join_ack_t;

typedef struct EMPACADO {
    uint64_t marca_ms;
    uint32_t libres;
    uint32_t copias;
    uint32_t carga;
    uint32_t reservado;
} msg_heartbeat_t;

typedef struct EMPACADO {
    uint16_t id;
    uint16_t reservado;
} msg_node_down_t;

typedef struct EMPACADO {
    uint32_t vpn;
    uint8_t  acceso;
    uint8_t  reservado[3];
    uint32_t version;
} msg_page_request_t;

typedef struct EMPACADO {
    uint32_t vpn;
    uint8_t  estado;            /* EST_S o EST_M */
    uint8_t  con_datos;
    uint16_t reservado;
    uint32_t version;           /* si con_datos, le sigue la pagina */
} msg_page_data_t;

typedef struct EMPACADO {
    uint32_t vpn;
    uint16_t nuevo_dueno;
    uint16_t reservado;
} msg_invalidate_t;

typedef struct EMPACADO {
    uint32_t vpn;
} msg_invalidate_ack_t;

typedef struct EMPACADO {
    uint32_t vpn;
    uint8_t  degradar_a;        /* EST_S o EST_I */
    uint8_t  reservado[3];
} msg_fetch_t;

typedef struct EMPACADO {
    uint32_t vpn;
    uint32_t version;
    uint32_t reservado;         /* le sigue la pagina */
} msg_fetch_reply_t;

typedef struct EMPACADO {
    uint32_t vpn_inicio;
    uint32_t cantidad;
    uint16_t hogar;
    uint16_t reservado;
} tramo_t;

typedef struct EMPACADO {
    uint32_t id;
    uint32_t vpn_base;
    uint32_t paginas;
    uint64_t tam_bytes;
    uint16_t creador;
    uint16_t cantidad_tramos;   /* le siguen los tramo_t */
    char     nombre[DSM_MAX_NOMBRE];
    uint32_t pid_creador;
} msg_segment_create_t;

typedef struct EMPACADO {
    uint32_t id;
} msg_segment_ack_t;

typedef struct EMPACADO {
    char     nombre[DSM_MAX_NOMBRE];
    uint32_t n;
    uint32_t generacion;
} msg_barrera_t;

typedef struct EMPACADO {
    int32_t  codigo;
    uint16_t tipo;
    uint16_t reservado;
} msg_error_t;

_Static_assert(sizeof(cabecera_t) == 24, "cabecera");
_Static_assert(sizeof(msg_join_t) == 52, "join");
_Static_assert(sizeof(entrada_nodo_t) == 24, "entrada_nodo");
_Static_assert(sizeof(msg_join_ack_t) == 8, "join_ack");
_Static_assert(sizeof(msg_heartbeat_t) == 24, "heartbeat");
_Static_assert(sizeof(msg_page_request_t) == 12, "page_request");
_Static_assert(sizeof(msg_page_data_t) == 12, "page_data");
_Static_assert(sizeof(msg_invalidate_t) == 8, "invalidate");
_Static_assert(sizeof(msg_fetch_t) == 8, "fetch");
_Static_assert(sizeof(msg_fetch_reply_t) == 12, "fetch_reply");
_Static_assert(sizeof(tramo_t) == 12, "tramo");
_Static_assert(sizeof(msg_segment_create_t) == 60, "segment_create");
_Static_assert(sizeof(msg_barrera_t) == 40, "barrera");
_Static_assert(sizeof(msg_error_t) == 8, "error");

/* ---- canal local libdsm <-> dsmd (misma maquina, orden del host) ---- */
enum {
    LOC_HOLA = 1, LOC_BIENVENIDA, LOC_FALLO, LOC_CONCESION,
    LOC_REVOCAR, LOC_REVOCADO, LOC_MALLOC, LOC_BUSCAR, LOC_BARRERA,
    LOC_INFO, LOC_CONSULTA, LOC_RESPUESTA, LOC_TEXTO
};

/* Un solo formato para todos los mensajes locales */
typedef struct {
    uint16_t tipo;
    int16_t  error;
    uint32_t id;                /* id de peticion o secuencia */
    uint32_t vpn;
    uint32_t ranura;            /* hilo de la app que fallo */
    uint32_t pid;
    uint8_t  acceso;            /* lectura/escritura, o PROT_* a aplicar */
    uint8_t  reservado[3];
    uint64_t valor;
    uint64_t valor2;
    char     nombre[DSM_MAX_NOMBRE];
} msg_local_t;
_Static_assert(sizeof(msg_local_t) == 72, "msg_local");

#endif

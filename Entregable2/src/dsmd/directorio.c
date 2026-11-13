/* directorio.c - protocolo MSI: el lado del hogar y el lado del nodo que falla */
#include <arpa/inet.h>
#include <string.h>
#include <sys/mman.h>
#include "dsmd.h"

#define BIT(n) (1u << (n))

static int prot_de(int estado)
{
    return estado == EST_M ? PROT_READ | PROT_WRITE : estado == EST_S ? PROT_READ : PROT_NONE;
}

static int satisface(int estado, int acceso)
{
    return acceso == ACCESO_ESCRITURA ? estado == EST_M : (estado == EST_S || estado == EST_M);
}

static int transitorio(int estado) { return estado >= EST_IS; }

/* Una copia invalidada mientras habia una escritura en camino pasa de SM a IM */
static int estado_tras_invalidar(int estado)
{
    if (estado == EST_SM) return EST_IM;
    return transitorio(estado) ? estado : EST_I;
}

/* Le pide la pagina al nodo que la tiene en M y deja la version vigente en el hogar */
static int traer_de_dueno(uint16_t dueno, uint32_t vpn, int degradar)
{
    if (dueno == g.yo) {
        franja_t *f = franja_de(vpn);
        pthread_mutex_lock(&f->mutex);
        pagina_obtener(vpn)->estado = degradar;
        pthread_cond_broadcast(&f->cambio);
        pthread_mutex_unlock(&f->mutex);
        return revocar_locales(vpn, prot_de(degradar));
    }
    msg_fetch_t fm = { .vpn = htonl(vpn), .degradar_a = degradar };
    uint32_t seq = pend_nueva(1);
    int r = red_enviar(dueno, MSG_FETCH, 0, seq, &fm, sizeof fm, NULL);
    if (r) pend_completar(seq, r, NULL);
    mensaje_t *resp;
    r = pend_esperar(seq, &resp);
    SUMAR(fetch_enviados, 1);
    if (!r && resp->len >= sizeof(msg_fetch_reply_t) + g.cfg.tam_pagina) {
        memcpy(dir_pagina(vpn), resp->cuerpo + sizeof(msg_fetch_reply_t), g.cfg.tam_pagina);
        SUMAR(paginas_recibidas, 1);
    } else if (!r) r = DSM_EPROTOCOLO;
    mensaje_liberar(resp);
    return r;
}

/* Invalida las copias en S y espera todas las confirmaciones */
static int invalidar_copias(uint32_t mascara, uint32_t vpn, uint16_t nuevo)
{
    int remotas = 0;
    for (int i = 0; i < MAX_NODOS; i++)
        if ((mascara & BIT(i)) && i != g.yo) remotas++;
    uint32_t seq = pend_nueva(remotas);
    msg_invalidate_t inv = { .vpn = htonl(vpn), .nuevo_dueno = htons(nuevo) };
    for (int i = 0; i < MAX_NODOS; i++) {
        if (!(mascara & BIT(i)) || i == g.yo) continue;
        SUMAR(invalidaciones_enviadas, 1);
        if (red_enviar(i, MSG_INVALIDATE, 0, seq, &inv, sizeof inv, NULL))
            pend_completar(seq, 0, NULL);       /* un nodo caido ya no tiene copia */
    }
    if (mascara & BIT(g.yo)) {
        franja_t *f = franja_de(vpn);
        pthread_mutex_lock(&f->mutex);
        entrada_pagina_t *e = pagina_obtener(vpn);
        e->estado = estado_tras_invalidar(e->estado);
        pthread_cond_broadcast(&f->cambio);
        pthread_mutex_unlock(&f->mutex);
        revocar_locales(vpn, PROT_NONE);
    }
    return pend_esperar(seq, NULL);
}

/* Lado del hogar. Las peticiones de una misma pagina se atienden de una en una
 * (marca 'ocupada'), y la respuesta sale antes de soltar la marca para que
 * ningun INVALIDATE posterior la adelante en la conexion. */
static int servir(uint32_t vpn, uint16_t sol, int acceso, uint32_t seq)
{
    franja_t *f = franja_de(vpn);
    pthread_mutex_lock(&f->mutex);
    entrada_pagina_t *e = pagina_obtener(vpn);
    if (!e || !e->es_hogar) {
        pthread_mutex_unlock(&f->mutex);
        return DSM_EINVAL;
    }
    while (e->ocupada) pthread_cond_wait(&f->cambio, &f->mutex);
    e->ocupada = 1;
    uint16_t dueno = e->dueno;
    uint32_t copias = e->copias;
    pthread_mutex_unlock(&f->mutex);

    int err = 0, concedido, con_datos = 1;
    uint16_t nuevo_dueno;
    if (acceso == ACCESO_LECTURA) {
        if (dueno != NADIE && dueno != sol) {
            err = traer_de_dueno(dueno, vpn, EST_S);
            copias |= BIT(dueno);
        }
        if (dueno == sol) { nuevo_dueno = sol; concedido = EST_M; con_datos = 0; }
        else { nuevo_dueno = NADIE; copias |= BIT(sol); concedido = EST_S; }
    } else {
        int tenia = (dueno == sol) || (dueno == NADIE && (copias & BIT(sol)));
        if (dueno != NADIE && dueno != sol) err = traer_de_dueno(dueno, vpn, EST_I);
        if (!err) err = invalidar_copias(copias & ~BIT(sol), vpn, sol);
        nuevo_dueno = sol;
        copias = 0;
        concedido = EST_M;
        con_datos = !tenia;
    }

    pthread_mutex_lock(&f->mutex);
    if (!err) {
        e->dueno = nuevo_dueno;
        e->copias = copias;
        if (acceso == ACCESO_ESCRITURA) e->version++;
        if (sol == g.yo) e->estado = concedido;
    }
    pthread_mutex_unlock(&f->mutex);

    if (sol != g.yo) {
        if (err) {
            msg_error_t me = { .codigo = htonl(err), .tipo = htons(MSG_PAGE_REQUEST) };
            red_enviar(sol, MSG_ERROR, BANDERA_RESP, seq, &me, sizeof me, NULL);
        } else {
            msg_page_data_t pd = { .vpn = htonl(vpn), .estado = concedido,
                                   .con_datos = con_datos, .version = htonl(e->version) };
            red_enviar(sol, MSG_PAGE_DATA, BANDERA_RESP, seq, &pd, sizeof pd,
                       con_datos ? dir_pagina(vpn) : NULL);
        }
    }

    pthread_mutex_lock(&f->mutex);
    e->ocupada = 0;
    pthread_cond_broadcast(&f->cambio);
    pthread_mutex_unlock(&f->mutex);
    return err;
}

void atender_page_request(mensaje_t *m)
{
    if (m->len < sizeof(msg_page_request_t)) { red_responder_error(m, DSM_EPROTOCOLO); mensaje_liberar(m); return; }
    msg_page_request_t *r = (msg_page_request_t *)m->cuerpo;
    int err = servir(ntohl(r->vpn), m->cab.origen, r->acceso, m->cab.secuencia);
    if (err == DSM_EINVAL) red_responder_error(m, err);
    mensaje_liberar(m);
}

/* Lado del nodo que falla: atiende un LOC_FALLO de un proceso local */
void resolver_fallo(mensaje_t *m)
{
    uint32_t vpn = m->loc.vpn;
    int acceso = m->loc.acceso, remoto = 0;
    uint64_t t0 = ahora_us();
    franja_t *f = franja_de(vpn);

    pthread_mutex_lock(&f->mutex);
    entrada_pagina_t *e = pagina_obtener(vpn);
    if (!e) {
        pthread_mutex_unlock(&f->mutex);
        conceder(m->fd, m->loc.ranura, vpn, PROT_NONE, DSM_EINVAL);
        mensaje_liberar(m);
        return;
    }
    for (;;) {
        if (satisface(e->estado, acceso)) {
            conceder(m->fd, m->loc.ranura, vpn, prot_de(e->estado), 0);
            e->fallos++;
            pthread_mutex_unlock(&f->mutex);
            if (remoto) {
                SUMAR(fallos_remotos, 1);
                SUMAR(latencia_total_us, ahora_us() - t0);
            } else SUMAR(fallos_locales, 1);
            mensaje_liberar(m);
            return;
        }
        if (transitorio(e->estado)) {           /* otro hilo ya la pidio: esperar */
            pthread_cond_wait(&f->cambio, &f->mutex);
            continue;
        }
        e->estado = e->estado == EST_S ? EST_SM : acceso == ACCESO_ESCRITURA ? EST_IM : EST_IS;
        uint16_t hogar = e->hogar;
        pthread_mutex_unlock(&f->mutex);
        remoto = 1;

        int err;
        if (hogar == g.yo) {
            err = servir(vpn, g.yo, acceso, 0);
        } else {
            msg_page_request_t r = { .vpn = htonl(vpn), .acceso = acceso };
            uint32_t seq = pend_nueva(1);
            int rr = red_enviar(hogar, MSG_PAGE_REQUEST, 0, seq, &r, sizeof r, NULL);
            if (rr) pend_completar(seq, rr, NULL);
            err = pend_esperar(seq, NULL);
        }

        pthread_mutex_lock(&f->mutex);
        if (err) {
            if (transitorio(e->estado)) e->estado = e->estado == EST_SM ? EST_S : EST_I;
            pthread_cond_broadcast(&f->cambio);
            conceder(m->fd, m->loc.ranura, vpn, PROT_NONE, err);
            pthread_mutex_unlock(&f->mutex);
            LOG("no se pudo obtener la pagina %u del nodo %u: %s", vpn, hogar, dsm_strerror(err));
            mensaje_liberar(m);
            return;
        }
    }
}

/* Se atiende en el hilo de ordenes, en el mismo orden en que llego */
void atender_page_data(mensaje_t *m)
{
    msg_page_data_t *pd = (msg_page_data_t *)m->cuerpo;
    if (m->len >= sizeof *pd) {
        uint32_t vpn = ntohl(pd->vpn);
        franja_t *f = franja_de(vpn);
        pthread_mutex_lock(&f->mutex);
        entrada_pagina_t *e = pagina_obtener(vpn);
        if (e) {
            if (pd->con_datos && m->len >= sizeof *pd + g.cfg.tam_pagina) {
                memcpy(dir_pagina(vpn), m->cuerpo + sizeof *pd, g.cfg.tam_pagina);
                SUMAR(paginas_recibidas, 1);
            }
            e->estado = pd->estado;
            e->version = ntohl(pd->version);
            pthread_cond_broadcast(&f->cambio);
        }
        pthread_mutex_unlock(&f->mutex);
    }
    pend_completar(m->cab.secuencia, 0, NULL);
    mensaje_liberar(m);
}

/* La confirmacion sale solo despues de que los procesos locales bajaron el permiso */
void atender_invalidate(mensaje_t *m)
{
    msg_invalidate_t *inv = (msg_invalidate_t *)m->cuerpo;
    uint32_t vpn = ntohl(inv->vpn);
    franja_t *f = franja_de(vpn);
    pthread_mutex_lock(&f->mutex);
    entrada_pagina_t *e = pagina_obtener(vpn);
    int liberar = 0;
    if (e) {
        e->estado = estado_tras_invalidar(e->estado);
        liberar = !e->es_hogar;
        pthread_cond_broadcast(&f->cambio);
    }
    pthread_mutex_unlock(&f->mutex);
    revocar_locales(vpn, PROT_NONE);
    if (liberar) pagina_liberar_memoria(vpn);
    SUMAR(invalidaciones_recibidas, 1);

    msg_invalidate_ack_t a = { .vpn = htonl(vpn) };
    red_enviar(m->cab.origen, MSG_INVALIDATE_ACK, BANDERA_RESP, m->cab.secuencia, &a, sizeof a, NULL);
    mensaje_liberar(m);
}

void atender_fetch(mensaje_t *m)
{
    msg_fetch_t *fm = (msg_fetch_t *)m->cuerpo;
    uint32_t vpn = ntohl(fm->vpn);
    int degradar = fm->degradar_a == EST_S ? EST_S : EST_I;
    franja_t *f = franja_de(vpn);
    pthread_mutex_lock(&f->mutex);
    entrada_pagina_t *e = pagina_obtener(vpn);
    uint32_t version = 0;
    if (e) {
        e->estado = degradar;
        version = e->version;
        pthread_cond_broadcast(&f->cambio);
    }
    pthread_mutex_unlock(&f->mutex);
    revocar_locales(vpn, prot_de(degradar));

    msg_fetch_reply_t r = { .vpn = htonl(vpn), .version = htonl(version) };
    red_enviar(m->cab.origen, MSG_FETCH_REPLY, BANDERA_RESP, m->cab.secuencia, &r, sizeof r,
               dir_pagina(vpn));
    if (degradar == EST_I && e && !e->es_hogar) pagina_liberar_memoria(vpn);
    mensaje_liberar(m);
}

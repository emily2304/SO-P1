/* cola.c - buffer acotado con semaforos POSIX y tabla de peticiones pendientes */
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include "dsmd.h"

uint64_t ahora_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

uint64_t ahora_ms(void) { return ahora_us() / 1000; }

void cola_iniciar(cola_t *c, int tam)
{
    c->items = calloc(tam, sizeof *c->items);
    c->tam = tam;
    c->cabeza = c->cola = 0;
    sem_init(&c->huecos, 0, tam);
    sem_init(&c->llenos, 0, 0);
    pthread_mutex_init(&c->mutex, NULL);
}

void encolar(cola_t *c, mensaje_t *m)
{
    while (sem_wait(&c->huecos) < 0 && errno == EINTR) { }
    pthread_mutex_lock(&c->mutex);
    c->items[c->cola] = m;
    c->cola = (c->cola + 1) % c->tam;
    pthread_mutex_unlock(&c->mutex);
    sem_post(&c->llenos);
}

mensaje_t *desencolar(cola_t *c)
{
    while (sem_wait(&c->llenos) < 0 && errno == EINTR) { }
    pthread_mutex_lock(&c->mutex);
    mensaje_t *m = c->items[c->cabeza];
    c->cabeza = (c->cabeza + 1) % c->tam;
    pthread_mutex_unlock(&c->mutex);
    sem_post(&c->huecos);
    return m;
}

void mensaje_liberar(mensaje_t *m)
{
    if (!m) return;
    free(m->cuerpo);
    free(m);
}

/* Registra una peticion que espera 'faltan' respuestas y devuelve su secuencia */
uint32_t pend_nueva(int faltan)
{
    pthread_mutex_lock(&g.mx_pend);
    uint32_t seq;
    pendiente_t *p;
    do {
        seq = ++g.sig_seq;
        p = &g.pend[seq % MAX_PEND];
    } while (seq == 0 || p->activo);
    p->activo = 1;
    p->seq = seq;
    p->faltan = faltan;
    p->error = 0;
    p->resp = NULL;
    pthread_mutex_unlock(&g.mx_pend);
    return seq;
}

void pend_completar(uint32_t seq, int error, mensaje_t *resp)
{
    pthread_mutex_lock(&g.mx_pend);
    pendiente_t *p = &g.pend[seq % MAX_PEND];
    if (p->activo && p->seq == seq) {
        if (error) { p->error = error; p->faltan = 0; }
        else p->faltan--;
        if (resp && !p->resp) { p->resp = resp; resp = NULL; }
        if (p->faltan <= 0) pthread_cond_broadcast(&g.cv_pend);
    }
    pthread_mutex_unlock(&g.mx_pend);
    mensaje_liberar(resp);      /* respuesta tardia o sobrante */
}

/* Espera hasta que lleguen todas las respuestas o se venza el plazo */
int pend_esperar(uint32_t seq, mensaje_t **resp)
{
    struct timespec lim;
    clock_gettime(CLOCK_REALTIME, &lim);
    lim.tv_sec += g.cfg.plazo_peticion / 1000;
    lim.tv_nsec += (g.cfg.plazo_peticion % 1000) * 1000000L;
    if (lim.tv_nsec >= 1000000000L) { lim.tv_sec++; lim.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g.mx_pend);
    pendiente_t *p = &g.pend[seq % MAX_PEND];
    int r = 0;
    while (p->faltan > 0 && r != ETIMEDOUT)
        r = pthread_cond_timedwait(&g.cv_pend, &g.mx_pend, &lim);
    int error = p->faltan > 0 ? DSM_ETIEMPO : p->error;
    mensaje_t *m = p->resp;
    p->activo = 0;
    p->resp = NULL;
    pthread_mutex_unlock(&g.mx_pend);

    if (resp) *resp = m;
    else mensaje_liberar(m);
    return error;
}

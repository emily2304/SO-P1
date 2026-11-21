/* pager.c - manejador de SIGSEGV e hilo de servicio de la aplicacion */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <ucontext.h>
#include <unistd.h>
#include "libdsm.h"

#define MAX_RANURAS 256

static int tubos[MAX_RANURAS][2];      /* un pipe por hilo que alguna vez fallo */
static int sig_ranura;
static __thread int mi_ranura = -1;
static struct sigaction anterior;
static pthread_t servicio;
static pid_t mi_pid;

static void escribir(const char *s) { if (write(2, s, strlen(s)) < 0) { } }

/* Solo usa funciones seguras para senales: send, read, write, pipe y sigaction */
static void manejador(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    int errno_guardado = errno;
    uint8_t *dir = si->si_addr;
    if (dir < dsm_base || dir >= dsm_base + dsm_tam_espacio) {
        sigaction(SIGSEGV, &anterior, NULL);    /* fallo real del programa */
        return;
    }
    if (mi_ranura < 0) {
        int r = __atomic_fetch_add(&sig_ranura, 1, __ATOMIC_SEQ_CST);
        if (r >= MAX_RANURAS || pipe(tubos[r]) < 0) {
            escribir("dsm: demasiados hilos usando el DSM\n");
            _exit(1);
        }
        mi_ranura = r;
    }
    msg_local_t m;
    m.tipo = LOC_FALLO;
    m.error = 0;
    m.vpn = (uint32_t)((dir - dsm_base) / dsm_tam_pagina);
    m.ranura = mi_ranura;
    m.pid = mi_pid;
#if defined(__x86_64__)
    ucontext_t *uc = ctx;
    m.acceso = (uc->uc_mcontext.gregs[REG_ERR] & 0x2) ? ACCESO_ESCRITURA : ACCESO_LECTURA;
#else
    (void)ctx;
    m.acceso = ACCESO_ESCRITURA;        /* sin el codigo de error se pide la pagina en M */
#endif
    if (send(dsm_sock, &m, sizeof m, MSG_NOSIGNAL) < 0) {
        escribir("dsm: se perdio la conexion con el dsmd\n");
        _exit(1);
    }
    char res = 1;
    while (read(tubos[mi_ranura][0], &res, 1) < 0 && errno == EINTR) { }
    if (res != 0) {
        escribir("dsm: no se pudo obtener una pagina del sistema (ver la bitacora del dsmd)\n");
        struct sigaction def = { .sa_handler = SIG_DFL };
        sigaction(SIGSEGV, &def, NULL);
    }
    errno = errno_guardado;             /* al volver se reintenta la instruccion */
}

/* Recibe del dsmd las concesiones, revocaciones y respuestas del API */
static void *hilo_servicio(void *arg)
{
    (void)arg;
    msg_local_t m;
    while (recv(dsm_sock, &m, sizeof m, 0) == (ssize_t)sizeof m) {
        uint8_t *pag = dsm_base + (uint64_t)m.vpn * dsm_tam_pagina;
        if (m.tipo == LOC_CONCESION) {
            char res = m.error != 0;
            if (!res && mprotect(pag, dsm_tam_pagina, m.acceso) < 0) res = 1;
            if (m.ranura < MAX_RANURAS && write(tubos[m.ranura][1], &res, 1) < 0) { }
        } else if (m.tipo == LOC_REVOCAR) {
            /* valor2 > 1: un rango completo (dsm_free); si no, una sola pagina */
            uint64_t n = m.valor2 > 1 ? m.valor2 : 1;
            mprotect(pag, n * dsm_tam_pagina, m.acceso);
            m.tipo = LOC_REVOCADO;
            send(dsm_sock, &m, sizeof m, MSG_NOSIGNAL);
        } else if (m.tipo == LOC_RESPUESTA) {
            api_entregar_respuesta(&m);
        }
    }
    return NULL;
}

int pager_iniciar(void)
{
    mi_pid = getpid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = manejador;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &anterior) < 0) return -1;
    return pthread_create(&servicio, NULL, hilo_servicio, NULL);
}

void pager_terminar(void)
{
    shutdown(dsm_sock, SHUT_RDWR);
    pthread_join(servicio, NULL);
    sigaction(SIGSEGV, &anterior, NULL);
}

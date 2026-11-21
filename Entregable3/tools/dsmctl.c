/* dsmctl - consultas al dsmd local: nodos, paginas, segmentos y estadisticas.
 * Con -g la respuesta incluye a todos los nodos en linea (prototipo 2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "config.h"
#include "protocolo.h"

int main(int argc, char **argv)
{
    const char *conf = getenv("DSM_CONF") ? getenv("DSM_CONF") : "dsm.conf";
    int nodo = getenv("DSM_NODO") ? atoi(getenv("DSM_NODO")) : 0, global = 0, op;
    while ((op = getopt(argc, argv, "c:n:g")) != -1) {
        if (op == 'c') conf = optarg;
        else if (op == 'n') nodo = atoi(optarg);
        else if (op == 'g') global = 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "uso: %s [-c dsm.conf] [-n nodo] [-g] nodos|paginas|segmentos|estadisticas\n"
                        "  -n  nodo local al que se pregunta (por defecto DSM_NODO o 0)\n"
                        "  -g  vista global: la respuesta de cada nodo en linea\n", argv[0]);
        return 2;
    }
    config_t c;
    if (config_leer(conf, &c) != 0) { fprintf(stderr, "dsmctl: no se pudo leer %s\n", conf); return 1; }

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    config_ruta_socket(&c, nodo, a.sun_path, sizeof a.sun_path);
    int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "dsmctl: no hay un dsmd en %s\n", a.sun_path);
        return 1;
    }
    msg_local_t m = { .tipo = LOC_CONSULTA, .pid = getpid(), .valor = global };
    strncpy(m.nombre, argv[optind], DSM_MAX_NOMBRE - 1);
    send(s, &m, sizeof m, 0);
    if (recv(s, &m, sizeof m, 0) != (ssize_t)sizeof m || m.tipo != LOC_TEXTO) return 1;

    char *buf = malloc(32768);
    for (uint64_t leido = 0; leido < m.valor; ) {
        ssize_t r = recv(s, buf, 32768, 0);
        if (r <= 0) break;
        fwrite(buf, 1, r, stdout);
        leido += r;
    }
    free(buf);
    close(s);
    return 0;
}

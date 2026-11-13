#include "dsm.h"

const char *dsm_strerror(int codigo)
{
    switch (codigo) {
    case DSM_OK:          return "sin error";
    case DSM_ECONF:       return "archivo de configuracion ausente o invalido";
    case DSM_ENODEMONIO:  return "no hay un dsmd corriendo en este nodo";
    case DSM_ENOMEM:      return "no alcanza la memoria aportada por el cluster";
    case DSM_EINVAL:      return "argumento invalido";
    case DSM_ENOENT:      return "segmento inexistente";
    case DSM_EEXISTE:     return "ya existe un segmento con ese nombre";
    case DSM_ETIEMPO:     return "se vencio el plazo de espera";
    case DSM_ERED:        return "error de comunicacion";
    case DSM_ENODOCAIDO:  return "el nodo necesario no responde";
    case DSM_EPROTOCOLO:  return "mensaje mal formado o version incompatible";
    case DSM_ENOINICIADO: return "se llamo al API antes de dsm_iniciar";
    }
    return "error desconocido";
}

#ifndef ANONFB_HANDLERS_H
#define ANONFB_HANDLERS_H

#include <microhttpd.h>

#include "http.h"

enum MHD_Result handle_health(request_t *req);
enum MHD_Result handle_meta(request_t *req);
enum MHD_Result handle_submit_feedback(request_t *req);

enum MHD_Result handle_admin_list(request_t *req);
enum MHD_Result handle_admin_get(request_t *req, const char *id);
enum MHD_Result handle_admin_update(request_t *req, const char *id);
enum MHD_Result handle_admin_delete(request_t *req, const char *id);

#endif /* ANONFB_HANDLERS_H */

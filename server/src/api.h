/* REST API: dispatcher and handler declarations (mounted on /api/v1). */
#ifndef FSS_API_H
#define FSS_API_H

#include "http.h"

/* on_http callback for the "/api/v1" prefix route. */
void fss_api_dispatch(fio_http_s *h);

/* api_meta.c */
void api_health(fio_http_s *h, fss_params_s *p);
void api_campuses_index(fio_http_s *h, fss_params_s *p);
void api_campus_show(fio_http_s *h, fss_params_s *p);

/* api_auth.c */
void api_auth_register(fio_http_s *h, fss_params_s *p);
void api_auth_login(fio_http_s *h, fss_params_s *p);
void api_auth_logout(fio_http_s *h, fss_params_s *p);
void api_me_show(fio_http_s *h, fss_params_s *p);
void api_me_update(fio_http_s *h, fss_params_s *p);

/* api_spots.c */
void api_spots_index(fio_http_s *h, fss_params_s *p);
void api_spots_create(fio_http_s *h, fss_params_s *p);
void api_spot_show(fio_http_s *h, fss_params_s *p);
void api_spot_claims_create(fio_http_s *h, fss_params_s *p);
void api_claim_vote(fio_http_s *h, fss_params_s *p);

/* api_reports.c */
void api_spot_reports_create(fio_http_s *h, fss_params_s *p);

/* api_checkins.c */
void api_checkins_create(fio_http_s *h, fss_params_s *p);
void api_checkin_heartbeat(fio_http_s *h, fss_params_s *p);
void api_checkin_end(fio_http_s *h, fss_params_s *p);

/* api_community.c */
void api_spot_confirm(fio_http_s *h, fss_params_s *p);
void api_spot_photos_create(fio_http_s *h, fss_params_s *p);
void api_photo_vote(fio_http_s *h, fss_params_s *p);
void api_me_karma(fio_http_s *h, fss_params_s *p);
void api_debug_job(fio_http_s *h, fss_params_s *p);

/* Folder for uploaded photos (set once at startup). */
extern const char *FSS_UPLOADS_DIR;

#endif /* FSS_API_H */

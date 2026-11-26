/* without runtime/{mbedtls,nghttp2}/<os> (tools/build_net_libs.py) this PC cannot host online play */
#include "rt.h"
u16 online_port(void){ return 47443; }
void online_server_start(void){ LOG("online: this build cannot host (no TLS/HTTP2 libraries)
"); }

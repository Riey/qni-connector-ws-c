#pragma once
#include <qni/qni-connector-context.h>

int qni_connector_ws_start(QniConnectorContext *ctx, const char *host, uint16_t port, int epoll_size);

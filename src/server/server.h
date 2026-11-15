#ifndef SERVER_H
#define SERVER_H

#include "persistence/persistence.h"

#define MAX_CLIENTS 64

/* SIGINT/SIGTERM에 종료 표시만 남기고 SIGPIPE는 무시한다. */
bool server_install_signal_handlers(void);
/* 리스너와 저장소의 소유권은 호출자에게 있다. 클라이언트는 종료 시 정리한다. */
int server_run(int listener, Store *store, Persistence *persistence);

#endif

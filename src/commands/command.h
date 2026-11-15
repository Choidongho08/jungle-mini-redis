#ifndef COMMAND_H
#define COMMAND_H

#include "protocol/resp.h"
#include "persistence/persistence.h"

/* 이벤트 루프에서 동기적으로 호출한다. 반환된 응답 버퍼는 호출자가 소유한다. */
Reply command_execute(Store *store, const Request *request, int64_t now,
                      Persistence *persistence);

#endif

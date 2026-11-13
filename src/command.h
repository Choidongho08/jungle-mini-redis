#ifndef MINI_REDIS_COMMAND_H
#define MINI_REDIS_COMMAND_H

#include "server.h"

/* c->argv[0] 의 명령을 찾아 실행하고, 응답을 c->reply 에 쌓는다.
 * 알 수 없는 명령이나 인자 개수 오류는 에러 응답을 보내고 연결은 유지한다 (FR-006). */
void command_execute(client_t *c);

#endif

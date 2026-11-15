#include "common/net.h"

#include <fcntl.h>

/* 읽거나 쓸 준비가 안 된 소켓 때문에 서버 전체가 대기하지 않도록 설정한다. */
bool net_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

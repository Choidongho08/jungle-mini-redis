#include "miniredis/status.h"

const char *mr_status_str(mr_status st) {
    switch (st) {
    case MR_OK:
        return "ok";
    case MR_NOT_FOUND:
        return "not found";
    case MR_COND_FAILED:
        return "condition not met";
    case MR_NOMEM:
        return "out of memory";
    case MR_INVALID_ARG:
        return "invalid argument";
    case MR_NOT_INTEGER:
        return "value is not an integer";
    case MR_OVERFLOW:
        return "integer overflow";
    case MR_IO_ERROR:
        return "i/o error";
    case MR_CORRUPT:
        return "corrupt data";
    }
    return "unknown status";
}

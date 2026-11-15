#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "storage/store.h"

typedef struct Persistence Persistence;

/* The directory must exist. Hold the log lock until persistence_close(). */
Persistence *persistence_open(const char *directory, Store *store);
void persistence_close(Persistence *persistence);
bool persistence_save(Persistence *persistence, const Store *store);
void persistence_record_set(Persistence *persistence, Bytes key, Bytes value,
                            int64_t deadline);
void persistence_record_delete(Persistence *persistence, const Bytes *keys, size_t count);

#endif

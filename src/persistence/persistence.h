#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "storage/store.h"

typedef struct Persistence Persistence;

/* The directory must exist. Hold its persistence lock until persistence_close(). */
Persistence *persistence_open(const char *directory, Store *store);
void persistence_close(Persistence *persistence);
bool persistence_save(Persistence *persistence, const Store *store);

#endif

/* 비교용 구현: 한 버킷을 방문하되, 긴 체인은 노드 예산만큼만 이동한다. */
#define REHASH_NODE_BUDGET 16

static void rehash_step_budget(Store *store, size_t node_budget)
{
    if (!store->new_buckets || node_budget == 0)
        return;

    Entry **head = &store->buckets[store->rehash_index];
    size_t moved = 0;
    while (*head && moved < node_budget) {
        Entry *entry = *head;
        *head = entry->next;
        size_t index = bucket_index(entry->hash, store->new_capacity);
        entry->next = store->new_buckets[index];
        store->new_buckets[index] = entry;
        moved++;
    }

    /* 남은 체인이 있으면 같은 버킷에서 다음 호출을 이어간다. */
    if (*head)
        return;

    /* 빈 버킷도 한 번에 하나만 방문해 기존의 탐색 정책을 유지한다. */
    store->rehash_index++;
    if (store->rehash_index == store->capacity) {
        free(store->buckets);
        store->buckets = store->new_buckets;
        store->capacity = store->new_capacity;
        store->new_buckets = NULL;
        store->new_capacity = 0;
        store->rehash_index = 0;
        store->sweep_index = 0;
    }
}

static void rehash_step(Store *store)
{
    rehash_step_budget(store, REHASH_NODE_BUDGET);
}

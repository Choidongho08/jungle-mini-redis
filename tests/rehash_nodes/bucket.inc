/* 빈 버킷도 한 단계로 세어 긴 빈 구간을 한꺼번에 순회하지 않는다.
 * 버킷 하나의 체인 전체를 옮기므로 Entry 수나 실행 시간의 제한은 아니다. */
static void rehash_step(Store *store)
{
    if (!store->new_buckets)
        return;
    Entry *entry = store->buckets[store->rehash_index];
    store->buckets[store->rehash_index] = NULL;
    while (entry) {
        Entry *next = entry->next;
        size_t index = bucket_index(entry->hash, store->new_capacity);
        entry->next = store->new_buckets[index];
        store->new_buckets[index] = entry;
        entry = next;
    }
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

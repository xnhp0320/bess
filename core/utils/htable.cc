#include "htable.h"

/* from the stored key pointer, return its value pointer */
inline void *HTableBase::key_to_value(const void *key) const {
  return (void *)((char *)key + value_offset_);
}

/* actually works faster for very small tables */
inline ht_keyidx_t HTableBase::_get_keyidx(uint32_t pri) const {
  struct ht_bucket *bucket = &buckets_[pri & bucket_mask_];

  for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
    if (pri == bucket->hv[i]) return bucket->keyidx[i];
  }

  uint32_t sec = ht_hash_secondary(pri);
  bucket = &buckets_[sec & bucket_mask_];
  for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
    if (pri == bucket->hv[i]) return bucket->keyidx[i];
  }

  return INVALID_KEYIDX;
}

void HTableBase::push_free_keyidx(ht_keyidx_t idx) {
  assert(0 <= idx && idx < num_entries_);

  *(ht_keyidx_t *)((uintptr_t)entries_ + entry_size_ * idx) = free_keyidx_;
  free_keyidx_ = idx;
}

/* entry array grows much more gently (50%) than bucket array (100%),
 * since space efficiency may be important for large keys and/or values. */
int HTableBase::expand_entries() {
  ht_keyidx_t old_size = num_entries_;
  ht_keyidx_t new_size = old_size + old_size / 2;

  void *new_entries;

  new_entries = mem_realloc(entries_, new_size * entry_size_);
  if (!new_entries) return -ENOMEM;

  num_entries_ = new_size;
  entries_ = new_entries;

  for (ht_keyidx_t i = new_size - 1; i >= old_size; i--) push_free_keyidx(i);

  return 0;
}

ht_keyidx_t HTableBase::pop_free_keyidx() {
  ht_keyidx_t ret = free_keyidx_;

  if (ret == INVALID_KEYIDX) {
    ret = expand_entries();
    if (ret) return ret;

    ret = free_keyidx_;
  }

  free_keyidx_ = get_next(ret);

  return ret;
}

/* returns an empty slot ID, or -ENOSPC */
static int find_empty_slot(const struct ht_bucket *bucket) {
  for (int i = 0; i < ENTRIES_PER_BUCKET; i++)
    if (bucket->hv[i] == 0) return i;

  return -ENOSPC;
}

/* Recursive function to try making an empty slot in the bucket.
 * Returns a slot ID in [0, ENTRIES_PER_BUCKET) for successful operation,
 * or -ENOSPC if failed */
int HTableBase::make_space(struct ht_bucket *bucket, int depth) {
  if (depth >= MAX_CUCKOO_PATH) return -ENOSPC;

  /* Something is wrong if there's already an empty slot in this bucket */
  assert(find_empty_slot(bucket) == -ENOSPC);

  for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
    void *key = keyidx_to_ptr(bucket->keyidx[i]);
    uint32_t pri = hash_nonzero(key);
    uint32_t sec = ht_hash_secondary(pri);
    struct ht_bucket *alt_bucket;
    int j;

    /* this entry is in its primary bucket? */
    if (pri == bucket->hv[i])
      alt_bucket = &buckets_[sec & bucket_mask_];
    else if (sec == bucket->hv[i])
      alt_bucket = &buckets_[pri & bucket_mask_];
    else
      assert(0);

    j = find_empty_slot(alt_bucket);
    if (j == -ENOSPC) j = make_space(alt_bucket, depth + 1);

    if (j >= 0) {
      /* Yay, we found one. Push recursively... */
      alt_bucket->hv[j] = bucket->hv[i];
      alt_bucket->keyidx[j] = bucket->keyidx[i];
      bucket->hv[i] = 0;
      return i;
    }
  }

  return -ENOSPC;
}

/* -ENOSPC if the bucket is full, 0 for success */
int HTableBase::add_to_bucket(struct ht_bucket *bucket, const void *key,
                                      const void *value) {
  for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
    if (bucket->hv[i] == 0) {
      void *entry;
      ht_keyidx_t k_idx = pop_free_keyidx();

      bucket->hv[i] = hash_nonzero(key);
      bucket->keyidx[i] = k_idx;

      entry = keyidx_to_ptr(k_idx);
      memcpy(entry, key, key_size_);
      memcpy((void *)((uintptr_t)entry + value_offset_), value, value_size_);

      cnt_++;
      return 0;
    }
  }

  return -ENOSPC;
}

/* the key must not already exist in the hash table */
int HTableBase::add_entry(uint32_t pri, uint32_t sec, const void *key,
                                  const void *value) {
  struct ht_bucket *pri_bucket;
  struct ht_bucket *sec_bucket;

again:
  pri_bucket = &buckets_[pri & bucket_mask_];
  if (add_to_bucket(pri_bucket, key, value) == 0) return 0;

  /* empty space in the secondary bucket? */
  sec_bucket = &buckets_[sec & bucket_mask_];
  if (add_to_bucket(sec_bucket, key, value) == 0) return 0;

  /* try kicking out someone in the primary bucket. */
  if (make_space(pri_bucket, 0) >= 0) goto again;

  /* try again from the secondary bucket */
  if (make_space(sec_bucket, 0) >= 0) goto again;

  return -ENOSPC;
}

void *HTableBase::get_from_bucket(uint32_t pri, uint32_t hv,
                                       const void *key) const {
  uint32_t b_idx = hv & bucket_mask_;
  struct ht_bucket *bucket = &buckets_[b_idx];

  for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
    ht_keyidx_t k_idx;
    void *key_stored;

    if (pri != bucket->hv[i]) continue;

    k_idx = bucket->keyidx[i];
    key_stored = keyidx_to_ptr(k_idx);

    if (keycmp_func_(key, key_stored, key_size_) == 0)
      return (void *)((uintptr_t)key_stored + value_offset_);
  }

  return NULL;
}

int HTableBase::del_from_bucket(uint32_t pri, uint32_t hv,
                                        const void *key) {
  uint32_t b_idx = hv & bucket_mask_;
  struct ht_bucket *bucket = &buckets_[b_idx];

  for (int i = 0; i < ENTRIES_PER_BUCKET; i++) {
    ht_keyidx_t k_idx;
    void *key_stored;

    if (pri != bucket->hv[i]) continue;

    k_idx = bucket->keyidx[i];
    key_stored = keyidx_to_ptr(k_idx);

    if (keycmp_func_(key, key_stored, key_size_) == 0) {
      bucket->hv[i] = 0;
      push_free_keyidx(k_idx);
      cnt_--;
      return 0;
    }
  }

  return -ENOENT;
}

int HTableBase::InitEx(struct ht_params *params) {
  if (!params) return -EINVAL;

  if (params->key_size < 1) return -EINVAL;

  if (params->value_size < 0) return -EINVAL;

  if (params->key_align < 1 || params->key_align > 64) return -EINVAL;

  if (params->value_align < 0 || params->value_align > 64) return -EINVAL;

  if (params->value_size > 0 && params->value_align == 0) return -EINVAL;

  if (params->num_buckets < 1) return -EINVAL;

  if (params->num_buckets != align_ceil_pow2(params->num_buckets))
    return -EINVAL;

  if (params->num_entries < ENTRIES_PER_BUCKET) return -EINVAL;

  hash_func_ = params->hash_func ?: DEFAULT_HASH_FUNC;
  keycmp_func_ = params->keycmp_func ?: memcmp;

  bucket_mask_ = params->num_buckets - 1;

  cnt_ = 0;
  num_entries_ = params->num_entries;
  free_keyidx_ = INVALID_KEYIDX;

  key_size_ = params->key_size;
  value_size_ = params->value_size;
  value_offset_ = align_ceil(key_size_, std::max(1ul, params->value_align));
  entry_size_ = align_ceil(value_offset_ + value_size_, params->key_align);

  buckets_ = (struct ht_bucket *)mem_alloc((bucket_mask_ + 1) *
                                           sizeof(struct ht_bucket));
  if (!buckets_) return -ENOMEM;

  entries_ = mem_alloc(num_entries_ * entry_size_);
  if (!entries_) {
    mem_free(buckets_);
    return -ENOMEM;
  }

  for (ht_keyidx_t i = num_entries_ - 1; i >= 0; i--) push_free_keyidx(i);

  return 0;
}

int HTableBase::Init(size_t key_size, size_t value_size) {
  struct ht_params params = {};

  params.key_size = key_size;
  params.value_size = value_size;

  params.key_align = 1;

  if (value_size > 0 && value_size % 8 == 0)
    params.value_align = 8;
  else if (value_size > 0 && value_size % 4 == 0)
    params.value_align = 4;
  else if (value_size > 0 && value_size % 2 == 0)
    params.value_align = 2;
  else
    params.value_align = 1;

  params.num_buckets = INIT_NUM_BUCKETS;
  params.num_entries = INIT_NUM_ENTRIES;

  params.hash_func = NULL;
  params.keycmp_func = NULL;

  return InitEx(&params);
}

void HTableBase::Close() {
  mem_free(buckets_);
  mem_free(entries_);
  memset(this, 0, sizeof(*this));
}

void HTableBase::Clear() {
  uint32_t next = 0;
  void *key;

  while ((key = Iterate(&next))) Del(key);
}

void *HTableBase::Get(const void *key) const {
  uint32_t pri = hash(key);

  return GetHash(pri, key);
}

void *HTableBase::GetHash(uint32_t pri, const void *key) const {
  void *ret;

  pri = ht_make_nonzero(pri);

  /* check primary bucket */
  ret = get_from_bucket(pri, pri, key);
  if (ret) return ret;

  /* check secondary bucket */
  return get_from_bucket(pri, ht_hash_secondary(pri), key);
}

int HTableBase::clone_table(HTableBase *t_old,
                                    uint32_t num_buckets,
                                    ht_keyidx_t num_entries) {
  uint32_t next = 0;
  void *key;

  *this = *t_old;

  buckets_ =
      (struct ht_bucket *)mem_alloc(num_buckets * sizeof(struct ht_bucket));
  if (!buckets_) return -ENOMEM;

  entries_ = mem_alloc(num_entries * entry_size_);
  if (!entries_) {
    mem_free(buckets_);
    return -ENOMEM;
  }

  bucket_mask_ = num_buckets - 1;
  cnt_ = 0;
  num_entries_ = num_entries;
  free_keyidx_ = INVALID_KEYIDX;

  for (ht_keyidx_t i = num_entries_ - 1; i >= 0; i--) push_free_keyidx(i);

  while ((key = t_old->Iterate(&next))) {
    void *value = t_old->key_to_value(key);
    int ret = Set(key, value);

    if (ret) {
      Close();
      return ret;
    }
  }

  return 0;
}

/* may be called recursively */
int HTableBase::expand_buckets() {
  HTableBase *t = new HTableBase;
  uint32_t num_buckets = (bucket_mask_ + 1) * 2;

  assert(num_buckets == align_ceil_pow2(num_buckets));

  int ret = t->clone_table(this, num_buckets, num_entries_);
  if (ret == 0) {
    Close();
    *this = *t;
  }

  return ret;
}

int HTableBase::Set(const void *key, const void *value) {
  uint32_t pri = hash(key);
  uint32_t sec = ht_hash_secondary(pri);

  int ret = 0;

  /* If the key already exists, its value is updated with the new one */
  void *old_value = GetHash(pri, key);
  if (old_value) {
    memcpy(old_value, value, value_size_);
    return 1;
  }

  pri = ht_make_nonzero(pri);
  sec = ht_hash_secondary(pri);

  while (add_entry(pri, sec, key, value) < 0) {
    /* expand the table as the last resort */
    ret = expand_buckets();
    if (ret < 0) break;
    /* retry on the newly expanded table */
  }

  return ret;
}

int HTableBase::Del(const void *key) {
  uint32_t pri = hash_nonzero(key);
  uint32_t sec;

  if (del_from_bucket(pri, pri, key) == 0) return 0;

  sec = ht_hash_secondary(pri);
  if (del_from_bucket(pri, sec, key) == 0) return 0;

  return -ENOENT;
}

void *HTableBase::Iterate(uint32_t *next) const {
  uint32_t idx = *next;

  uint32_t i;
  int j;

  do {
    i = idx / ENTRIES_PER_BUCKET;
    j = idx % ENTRIES_PER_BUCKET;

    if (i >= bucket_mask_ + 1) {
      *next = idx;
      return NULL;
    }

    idx++;
  } while (buckets_[i].hv[j] == 0);

  *next = idx;
  return keyidx_to_ptr(buckets_[i].keyidx[j]);
}

int HTableBase::Count() const {
  return cnt_;
}

int HTableBase::count_entries_in_pri_bucket() const {
  int ret = 0;

  for (uint32_t i = 0; i < bucket_mask_ + 1; i++) {
    for (int j = 0; j < ENTRIES_PER_BUCKET; j++) {
      uint32_t pri = buckets_[i].hv[j];
      if (pri && (pri & bucket_mask_) == i) ret++;
    }
  }

  return ret;
}

void HTableBase::Dump(int detail) const {
  int in_pri_bucket = count_entries_in_pri_bucket();

  printf("--------------------------------------------\n");

  if (detail) {
    for (uint32_t i = 0; i < bucket_mask_ + 1; i++) {
      printf("%4d:  ", i);

      for (int j = 0; j < ENTRIES_PER_BUCKET; j++) {
        uint32_t pri = buckets_[i].hv[j];
        uint32_t sec = ht_hash_secondary(pri);
        char type;

        if (!pri) {
          printf("  --------/-------- ----     ");
          continue;
        }

        if ((pri & bucket_mask_) == i) {
          if ((sec & bucket_mask_) != i)
            type = ' ';
          else
            type = '?';
        } else
          type = '!';

        printf("%c %08x/%08x %4d     ", type, pri, sec, buckets_[i].keyidx[j]);
      }

      printf("\n");
    }
  }

  printf("cnt = %d\n", cnt_);
  printf("entry array size = %d\n", num_entries_);
  printf("buckets = %d\n", bucket_mask_ + 1);
  printf("occupancy = %.1f%% (%.1f%% in primary buckets)\n",
         100.0 * cnt_ / ((bucket_mask_ + 1) * ENTRIES_PER_BUCKET),
         100.0 * in_pri_bucket / (cnt_ ?: 1));

  printf("key_size = %zu\n", key_size_);
  printf("value_size = %zu\n", value_size_);
  printf("value_offset = %zu\n", value_offset_);
  printf("entry_size = %zu\n", entry_size_);
  printf("\n");
}

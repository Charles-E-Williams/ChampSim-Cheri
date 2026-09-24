#include "bloom_filter.h"

uint64_t BloomFilter::hash(uint64_t key)
{
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53ULL;
  key ^= key >> 33;
  return key;
}

bool BloomFilter::test(champsim::address addr) const
{
  uint64_t cl = champsim::block_number{addr}.to<uint64_t>();
  uint64_t h1 = hash(cl) % SIZE;
  uint64_t h2 = hash(cl ^ 0xDEADBEEF) % SIZE;
  return bloom.test(h1) && bloom.test(h2);
}

void BloomFilter::add(champsim::address addr)
{
  uint64_t cl = champsim::block_number{addr}.to<uint64_t>();
  uint64_t h1 = hash(cl) % SIZE;
  uint64_t h2 = hash(cl ^ 0xDEADBEEF) % SIZE;
  bloom.set(h1);
  bloom.set(h2);
  insertions++;
  maybe_reset();
}

void BloomFilter::maybe_reset()
{
  if (insertions >= RESET_INTERVAL) {
    bloom.reset();
    insertions = 0;
  }
}
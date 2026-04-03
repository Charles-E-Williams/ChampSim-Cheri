#ifndef BLOOM_FILTER_H
#define BLOOM_FILTER_H

#include <bitset>
#include <cstdint>
#include "champsim.h"
#include "address.h"

class BloomFilter {
public:
    constexpr static unsigned SIZE = 8192;
    constexpr static uint64_t RESET_INTERVAL = 1024;

    BloomFilter() = default;

    bool test(champsim::address addr) const;
    void add(champsim::address addr);
    static uint64_t hash(uint64_t key);

private:
    std::bitset<SIZE> bloom{};
    uint64_t insertions = 0;

    void maybe_reset();
};

#endif
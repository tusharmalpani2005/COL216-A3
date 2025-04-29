#ifndef BUS_HPP
#define BUS_HPP
#include <cstdint>
#include <algorithm>

using namespace std;

struct Bus {
    uint64_t busy_until;
    Bus() : busy_until(0) {}
    bool free_at(uint64_t cycle) { return cycle >= busy_until; }
    void occupy(uint64_t cycle, uint64_t duration) {
        busy_until = max(busy_until, cycle) + duration;
    }
};

#endif
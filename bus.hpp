#ifndef BUS_HPP
#define BUS_HPP
#include <algorithm>
#include <cstdint>

using namespace std;

struct Bus {
    unsigned long long busy_until;
    Bus() : busy_until(0) {}
    bool free_at(unsigned long long cycle) { return cycle >= busy_until; }
    void occupy(unsigned long long cycle, unsigned long long duration) {
        busy_until = max(busy_until, cycle) + duration;
    }
};

#endif
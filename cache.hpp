#ifndef CACHE_HPP
#define CACHE_HPP

#include "types.hpp"

struct Cache {
    int S, A, B;
    vector<vector<Line>> sets;
    unsigned long long use_counter;

    Cache(int s, int E, int b) : S(1 << s), A(E), B(1 << b), use_counter(0) {
        sets.resize(S, vector<Line>(A));
    }

    int find_line(unsigned long long tag, int set) {
        for (int i = 0; i < A; i++) {
            if (sets[set][i].valid && sets[set][i].tag == tag &&
                sets[set][i].state != I) {
                return i;
            }
        }
        return -1;
    }

    int choose_victim(int set) {
        int victim = 0;
        unsigned long long min_used = UINT64_MAX;
        for (int i = 0; i < A; i++) {
            if (!sets[set][i].valid) return i;
            if (sets[set][i].last_used < min_used) {
                min_used = sets[set][i].last_used;
                victim = i;
            }
        }
        return victim;
    }

    void touch(int set, int idx) { sets[set][idx].last_used = use_counter++; }
};

#endif
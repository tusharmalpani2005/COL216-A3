#ifndef CACHE_HPP
#define CACHE_HPP

#include "types.hpp"

struct Cache {
    int S, A, B;
    vector<vector<Line>> sets;
    uint64_t use_counter;

    Cache(int s, int E, int b) : S(1 << s), A(E), B(1 << b), use_counter(0) {
        sets.resize(S, vector<Line>(A));
    }

    int find_line(uint32_t tag, int set) {
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
        uint64_t min_used = UINT64_MAX;
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

    // Debug function to print cache state
    void print_set_state(int core, int set) {
        cout << "Cache[" << core << "] Set[" << set << "] State: ";
        for (int i = 0; i < A; i++) {
            if (sets[set][i].valid) {
                cout << "[" << i << ":0x" << hex << sets[set][i].tag << dec
                     << "(" << StateNames.at(sets[set][i].state) << ")] ";
            } else {
                cout << "[" << i << ":invalid] ";
            }
        }
        cout << endl;
    }
};

#endif
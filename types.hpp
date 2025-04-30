#ifndef TYPES_HPP
#define TYPES_HPP

#include <map>
#include <string>

using namespace std;

enum State { I, S, E, M };

struct Line {
    bool valid;
    State state;
    unsigned int tag;
    unsigned long long last_used;
    Line() : valid(false), state(I), tag(0), last_used(0) {}
};

struct PendingAllocation {
    int core;
    int set;
    int victim;
    int tag;
    State state;
    unsigned long long complete_cycle;
};

struct StallRequest {
    int core;
    unsigned long long until_cycle;
};

enum ChangeType { STATE_TRANSITION, INVALIDATION };

struct PlannedChange {
    int core;
    unsigned int set;
    int idx;
    bool valid;
    State state;
    unsigned int tag;
    unsigned long long last_used;
    unsigned long long apply_cycle;
    ChangeType type;
};

#endif
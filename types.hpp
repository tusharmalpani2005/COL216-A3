#ifndef TYPES_HPP
#define TYPES_HPP

#include <map>
#include <string>

using namespace std;

// Map for state names to improve readability in debug output
const map<int, string> StateNames = {{0, "I"}, {1, "S"}, {2, "E"}, {3, "M"}};

enum State { I, S, E, M };

struct Line {
    bool valid;
    State state;
    uint32_t tag;
    uint64_t last_used;
    Line() : valid(false), state(I), tag(0), last_used(0) {}
};

struct PendingAllocation {
    int core;
    int set;
    int victim;
    uint32_t tag;
    State state;
    uint64_t complete_cycle;
};

struct StallRequest {
    int core;
    uint64_t until_cycle;
};

enum ChangeType { STATE_TRANSITION, INVALIDATION };

struct PlannedChange {
    int core;
    uint32_t set;
    int idx;
    bool valid;
    State state;
    uint32_t tag;
    uint64_t last_used;
    uint64_t apply_cycle;
    ChangeType type;  // New field to differentiate changes
};

vector<StallRequest> stall_requests;

#endif
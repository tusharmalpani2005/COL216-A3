#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <queue>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <list>

using namespace std;

// MESI states
enum State { I, S, E, M };

// Cache line structure
struct Line {
    bool valid;
    State state;
    uint32_t tag;
    uint64_t last_used;  // for LRU
    Line(): valid(false), state(I), tag(0), last_used(0) {}
};

struct PendingAllocation {
    int core;           // Core index (c)
    int set;            // Set index
    int victim;         // Victim line index (v)
    uint32_t tag;       // Tag to set
    State state;        // MESI state to set
    uint64_t complete_cycle; // Cycle when allocation should occur
};

// Per-core cache structure
struct Cache {
    int S, A, B;
    vector<vector<Line>> sets;
    uint64_t use_counter;

    Cache(int s, int E, int b): S(1<<s), A(E), B(1<<b), use_counter(0) {
        for(int i = 0; i < S; i++) {
            auto v = vector<Line>(A);
            sets.push_back(v);
        }
    }

    // Find a line in the cache; return index or -1 if not found
    int find_line(uint32_t tag, int set) {
        for(int i = 0; i < A; i++) { // Fixed: Use A instead of E
            if(sets[set][i].valid && sets[set][i].tag == tag) {
                return i;
            }
        }
        return -1;
    }

    // Choose a victim line for eviction (LRU policy)
    int choose_victim(int set) {
        int victim = 0;
        uint64_t min_used = UINT64_MAX;
        for(int i = 0; i < A; i++) { // Fixed: Use A instead of E
            if(!sets[set][i].valid) return i;
            if(sets[set][i].last_used < min_used) {
                min_used = sets[set][i].last_used;
                victim = i;
            }
        }
        return victim;
    }

    // Update LRU timestamp
    void touch(int set, int idx) {
        sets[set][idx].last_used = use_counter++;
    }

    bool is_set_full(int set) {
        for(int i = 0; i < A; i++) { // Fixed: Use A instead of E
            if(!sets[set][i].valid) return false;
        }
        return true;
    }
};

// Single bus for arbitration
struct Bus {
    uint64_t busy_until;
    Bus(): busy_until(0) {}
    bool free_at(uint64_t cycle) { return cycle >= busy_until; }
    void occupy(uint64_t cycle, uint64_t duration) {
        busy_until = max(busy_until, cycle) + duration;
    }
};

// Per-core statistics
struct Stats {
    uint64_t instr = 0, reads = 0, writes = 0;
    uint64_t cycles = 0, idle = 0;
    uint64_t misses = 0, evictions = 0, writebacks = 0;
    uint64_t invalidations = 0, traffic = 0;
};

// A single memory reference
struct Ref { char type; uint32_t addr; };

int main(int argc, char* argv[]) {
    vector<string> traces(4);
    string pref;
    int s = 0, E = 0, b = 0;
    string outfn;

    // Parse command-line arguments
    for(int i = 1; i < argc; i++) {
        string a = argv[i];
        if(a == "-T" && i + 1 < argc) pref = argv[++i];
        else if(a == "-s" && i + 1 < argc) s = stoi(argv[++i]);
        else if(a == "-E" && i + 1 < argc) E = stoi(argv[++i]);
        else if(a == "-b" && i + 1 < argc) b = stoi(argv[++i]);
        else if(a == "-o" && i + 1 < argc) outfn = argv[++i];
        else if(a == "-h") {
            cout << "-T <tracefile>: name of parallel application (e.g. app1) whose 4 traces are to be used in simulation\n";
            cout << "-s <s>: number of set index bits (number of sets = 2^s)\n";
            cout << "-E <E>: associativity (number of cache lines per set)\n";
            cout << "-b <b>: number of block bits (block size = 2^b bytes)\n";
            cout << "-o <outfilename>: logs output in file for plotting etc.\n";
            cout << "-h: prints this help\n";
            return 0;
        }
    }

    // Build full filenames for traces
    for(int i = 0; i < 4; i++) {
        traces[i] = pref + "_proc" + to_string(i) + ".trace";
    }

    // Load traces into reference queues
    vector<deque<Ref>> refq(4);
    for(int c = 0; c < 4; c++) {
        ifstream f(traces[c]);
        if(!f) { cerr << "Cannot open " << traces[c] << "\n"; return 1; }
        char t; string addr;
        while(f >> t >> addr) {
            uint32_t a = stoul(addr, nullptr, 0);
            refq[c].push_back({t, a});
        }
    }

    // Setup caches, bus, and statistics
    vector<Cache> cache;
    for(int i = 0; i < 4; i++) cache.emplace_back(s, E, b);
    Bus bus;
    vector<Stats> st(4);
    vector<uint64_t> stall_until(4, 0);
    uint64_t global_cycle = 0;
    vector<PendingAllocation> pending_allocations;

    // Utility functions to extract set and tag from address
    auto get_set = [&](uint32_t addr) { return (addr >> b) & ((1u << s) - 1); };
    auto get_tag = [&](uint32_t addr) { return addr >> (s + b); };
    auto block_words = (1u << b) / 4u;  // words per block

    // Main simulation loop
    while(true) {
        
        bool done = true;
        // Check if all cores are finished and not stalled
        for(int c = 0; c < 4; c++) {
            if(!refq[c].empty() || global_cycle < stall_until[c]) {
                done = false; break;
            }
        }
        if(done && pending_allocations.empty()) break;

        for(auto it = pending_allocations.begin(); it != pending_allocations.end();) {
            if(global_cycle >= it->complete_cycle) {
                Cache &C = cache[it->core];
                int set = it->set, v = it->victim;
                C.sets[set][v].valid = true;
                C.sets[set][v].tag = it->tag;
                C.sets[set][v].state = it->state;
                C.touch(set, v);
                it = pending_allocations.erase(it); // Remove completed allocation
            } else {
                ++it;
            }
        }

        // Process each core
        for(int c = 0; c < 4; c++) {
            st[c].cycles++;
            if(global_cycle < stall_until[c]) {
                st[c].idle++;
                continue;
            }
            if(refq[c].empty()) continue;

            // Process one memory reference
            Ref R = refq[c].front();
            refq[c].pop_front();
            st[c].instr++;
            uint32_t set = get_set(R.addr), tag = get_tag(R.addr);
            bool isWrite = (R.type == 'W');
            if(isWrite) st[c].writes++;
            else st[c].reads++;

            Cache &C = cache[c];
            int idx = C.find_line(tag, set);
            if(idx >= 0 && C.sets[set][idx].state != I) {
                // Cache hit
                if(isWrite) {
                    if(C.sets[set][idx].state == M) {
                        // Already Modified, no bus action
                    } else if(C.sets[set][idx].state == E) {
                        C.sets[set][idx].state = M; // Silent transition from Exclusive to Modified
                    } else if(C.sets[set][idx].state == S) {
                        // Shared to Modified requires BusUpgr
                        // st[c].traffic += 0; // BusUpgr, no data transfer
                        for(int o = 0; o < 4; o++) if(o != c) {
                            int oi = cache[o].find_line(tag, set);
                            if(oi >= 0 && cache[o].sets[set][oi].state != I) {
                                cache[o].sets[set][oi].state = I;
                                st[c].invalidations++;
                            }
                        }
                        C.sets[set][idx].state = M;
                    }
                }
                C.touch(set, idx);
                continue;
            }

            // Cache miss
            st[c].misses++;
            bool isRdX = isWrite;
            if(!bus.free_at(global_cycle)) {
                refq[c].push_front(R);
                st[c].idle++;
                continue;
            }

            // Check other caches for the line
            bool found_shared = false;
            bool found_mod = false;

            for(int o = 0; o < 4; o++) if(o != c) {
                Cache &O = cache[o];
                int oi = O.find_line(tag, set);
                if(oi >= 0 && O.sets[set][oi].state != I) {
                    found_shared = true;
                    if(O.sets[set][oi].state == M) found_mod = true;
                }
            }

            // Determine appropriate state and bus transaction
            State new_state;
            uint64_t trans_cycles;

            if(isRdX) {  // Write miss: BusRdX
                new_state = M;
                trans_cycles = found_shared ? 2 * block_words : 100;  // 2 * block_words if supplied by another cache, 100 if from memory
                
                // Invalidate all other copies
                for(int o = 0; o < 4; o++) if(o != c) {
                    int oi = cache[o].find_line(tag, set);
                    if(oi >= 0 && cache[o].sets[set][oi].state != I) {
                        cache[o].sets[set][oi].state = I;
                        st[o].invalidations++;
                    }
                }
            } else {  // Read miss: BusRd
                if(found_shared) {
                    new_state = S;
                    trans_cycles = 2 * block_words;  // Data from another cache
                    
                    // Transition M or E in other caches to S
                    for(int o = 0; o < 4; o++) if(o != c) {
                        int oi = cache[o].find_line(tag, set);
                        if(oi >= 0 && (cache[o].sets[set][oi].state == M || cache[o].sets[set][oi].state == E)) {
                            cache[o].sets[set][oi].state = S;
                        }
                    }
                } else {
                    new_state = State::E;  // No other copies, go to Exclusive
                    trans_cycles = 100;    // Memory access
                }
            }
            st[c].traffic += block_words * 4;

            // Handle eviction if necessary
            int v = C.choose_victim(set);
            bool needs_writeback = false;
            if(C.sets[set][v].valid && C.sets[set][v].state == M) {
                needs_writeback = true;
                st[c].writebacks++;
                bus.occupy(global_cycle, 100);
                st[c].traffic += (1u << b);
            }
            if(C.sets[set][v].valid) {
                st[c].evictions++;
            }

            // Allocate new line
            PendingAllocation pa;
            pa.core = c;
            pa.set = set;
            pa.victim = v;
            pa.tag = tag;
            pa.state = new_state;  // Fixed: Use new_state instead of hardcoded logic
            pa.complete_cycle = global_cycle + (needs_writeback ? 100 + trans_cycles : trans_cycles);
            pending_allocations.push_back(pa);

            // Occupy bus and stall core
            bus.occupy(global_cycle, trans_cycles);
            stall_until[c] = bus.busy_until;
        }
        global_cycle++;
    }

    // Compute bus summary
    uint64_t total_bus_tx = 0, total_bus_traffic = 0;
    for(int c = 0; c < 4; c++) {
        total_bus_tx += st[c].invalidations; // Approximate bus transactions
        total_bus_traffic += st[c].traffic;
    }

    // Output results
    ostream* out = &cout;
    if(!outfn.empty()) {
        static ofstream f(outfn);
        if(!f) {
            cerr << "Cannot open output file: " << outfn << "\n";
            return 1;
        }
        out = &f;
    }
    *out << "Simulation Parameters:\n";
    *out << "Trace Prefix: " << pref << "\n";
    *out << "Set Index Bits: " << s << "\n";
    *out << "Associativity: " << E << "\n";
    *out << "Block Bits: " << b << "\n";
    *out << "Block Size (Bytes): " << (1 << b) << "\n";
    *out << "Number of Sets: " << (1 << s) << "\n";
    *out << "Cache Size (KB per core): " << ((1 << s) * E * (1 << b) / 1024) << "\n";
    *out << "MESI Protocol: Enabled\n";
    *out << "Write Policy: Write-back, Write-allocate\n";
    *out << "Replacement Policy: LRU\n";
    *out << "Bus: Central snooping bus\n\n";

    for(int c = 0; c < 4; c++) {
        double miss_rate = st[c].misses / double(st[c].reads + st[c].writes) * 100.0;
        *out << "Core " << c << " Statistics:\n";
        *out << "Total Instructions: " << st[c].instr << "\n";
        *out << "Total Reads: " << st[c].reads << "\n";
        *out << "Total Writes: " << st[c].writes << "\n";
        *out << "Total Execution Cycles: " << st[c].cycles << "\n";
        *out << "Idle Cycles: " << st[c].idle << "\n";
        *out << "Cache Misses: " << st[c].misses << "\n";
        *out << fixed << setprecision(2) << "Cache Miss Rate: " << miss_rate << "%\n";
        *out << "Cache Evictions: " << st[c].evictions << "\n";
        *out << "Writebacks: " << st[c].writebacks << "\n";
        *out << "Bus Invalidations: " << st[c].invalidations << "\n";
        *out << "Data Traffic (Bytes): " << st[c].traffic << "\n\n";
    }

    *out << "Overall Bus Summary:\n";
    *out << "Total Bus Transactions: " << total_bus_tx << "\n";
    *out << "Total Bus Traffic (Bytes): " << total_bus_traffic << "\n";

    return 0;
}
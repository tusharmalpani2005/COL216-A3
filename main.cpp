#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <chrono>
#include "cache.hpp"
#include "bus.hpp"

using namespace std;

struct Stats {
    unsigned long long instr = 0, reads = 0, writes = 0;
    unsigned long long execution_cycles = 0, idle = 0;
    unsigned long long misses = 0, evictions = 0, writebacks = 0;
    unsigned long long invalidations = 0, traffic = 0;
    bool waiting_for_own_request = false;
};

  struct Ref {
    char type;
    unsigned int addr;
};

// Function to format address for debug output
string format_addr(unsigned int addr, int s, int b) {
    stringstream ss;
    ss << "0x" << hex << addr << " (tag:0x" << (addr >> (s + b))
       << ", set:" << dec << ((addr >> b) & ((1u << s) - 1))
       << ", offset:" << (addr & ((1u << b) - 1)) << ")";
    return ss.str();
}

int main(int argc, char* argv[]) {
    vector<string> traces(4);
    string pref;
    int s = 5, E = 2, b = 5;
    string outfn;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "-t" && i + 1 < argc)
            pref = argv[++i];
        else if (a == "-s" && i + 1 < argc)
            s = stoi(argv[++i]);
        else if (a == "-E" && i + 1 < argc)
            E = stoi(argv[++i]);
        else if (a == "-b" && i + 1 < argc)
            b = stoi(argv[++i]);
        else if (a == "-o" && i + 1 < argc)
            outfn = argv[++i];
        else if (a == "-h") {
            cout << "-t <tracefile>: name of parallel application\n";
            cout << "-s <s>: number of set index bits\n";
            cout << "-E <E>: associativity\n";
            cout << "-b <b>: number of block bits\n";
            cout << "-o <outfilename>: logs output\n";
            cout << "-h: prints this help\n";
            return 0;
        }
    }

    for (int i = 0; i < 4; i++) {
        traces[i] = pref + "_proc" + to_string(i) + ".trace";
    }

    vector<deque<Ref>> refq(4);
    for (int c = 0; c < 4; c++) {
        ifstream f(traces[c]);
        if (!f) {
            cerr << "Cannot open " << traces[c] << "\n";
            return 1;
        }
        char t;
        string addr;
        while (f >> t >> addr) {
            unsigned int a = stoul(addr, nullptr, 0);
            refq[c].push_back({t, a});
        }
    }

    vector<Cache> cache(4, Cache(s, E, b));
    Bus bus;
    vector<Stats> st(4);
    vector<unsigned long long> stall_until(4, 0);
    unsigned long long global_cycle = 0;
    vector<PendingAllocation> pending_allocations;
    vector<PlannedChange> planned_changes;
    vector<StallRequest> stall_requests;

    auto get_set = [&](unsigned int addr) { return (addr >> b) & ((1u << s) - 1); };
    auto get_tag = [&](unsigned int addr) { return addr >> (s + b); };
    auto block_words = (1u << b) / 4u;

    auto start_time = chrono::high_resolution_clock::now();
    while (true) {
        bool done = true;
        for (int c = 0; c < 4; c++) {
            if (!refq[c].empty() || global_cycle < stall_until[c]) {
                done = false;
                break;
            }
        }
        if (done && pending_allocations.empty() && planned_changes.empty()) break;

        // First, apply any changes scheduled for this cycle
        vector<PlannedChange> next_planned_changes;
        for (auto& pc : planned_changes) {
            if (pc.apply_cycle <= global_cycle) {
                if (pc.type == STATE_TRANSITION) {
                    Cache& C = cache[pc.core];
                    C.sets[pc.set][pc.idx].valid = pc.valid;
                    C.sets[pc.set][pc.idx].state = pc.state;
                    C.sets[pc.set][pc.idx].tag = pc.tag;
                    C.sets[pc.set][pc.idx].last_used = pc.last_used;
                }
            } else {
                next_planned_changes.push_back(pc);
            }
        }
        for (auto& pc : planned_changes) {
            if (pc.apply_cycle <= global_cycle && pc.type == INVALIDATION) {
                Cache& C = cache[pc.core];
                //C.sets[pc.set][pc.idx].valid = pc.valid;
                C.sets[pc.set][pc.idx].state = pc.state;
                // C.sets[pc.set][pc.idx].tag = pc.tag;
                // C.sets[pc.set][pc.idx].last_used = pc.last_used;
            }
        }
        planned_changes = next_planned_changes;

        // Process pending allocations
        auto now_pending = std::move(pending_allocations);
        pending_allocations.clear();
        for (auto& pa : now_pending) {
            if (global_cycle >= pa.complete_cycle) {
                Cache& C = cache[pa.core];
                C.sets[pa.set][pa.victim].valid = true;
                C.sets[pa.set][pa.victim].tag = pa.tag;
                C.sets[pa.set][pa.victim].state = pa.state;
                C.touch(pa.set, pa.victim);
            } else {
                pending_allocations.push_back(pa);
            }
        }

        // Process memory references for each core
        for (int c = 0; c < 4; c++) {
            if (refq[c].empty()) continue;
            if (global_cycle < stall_until[c]) {
                if (!st[c].waiting_for_own_request) {
                    st[c].idle++;
                } else {
                    st[c].execution_cycles++;
                }
                continue;
            }

            st[c].execution_cycles++;
            
            Ref R = refq[c].front();
            unsigned int set = get_set(R.addr), tag = get_tag(R.addr);
            bool isWrite = (R.type == 'W');
            Cache& C = cache[c];
            int idx = C.find_line(tag, set);

            if (idx >= 0 && C.sets[set][idx].state != I) {
                if (isWrite) {
                    if (C.sets[set][idx].state == M) {
                        planned_changes.push_back(
                            {c, set, idx, true, M, tag, C.use_counter++, global_cycle + 1, STATE_TRANSITION});
                    } else if (C.sets[set][idx].state == E) {
                        planned_changes.push_back(
                            {c, set, idx, true, M, tag, C.use_counter++, global_cycle + 1, STATE_TRANSITION});
                    } else if (C.sets[set][idx].state == S) {
                        if (bus.free_at(global_cycle)) {
                            bool invalidated_others = false;
                            
                            for (int o = 0; o < 4; o++) {
                                if (o != c) {
                                    int oi = cache[o].find_line(tag, set);
                                    if (oi >= 0 && cache[o].sets[set][oi].state != I) {
                                        planned_changes.push_back({o, set, oi, false, I, cache[o].sets[set][oi].tag, 0, global_cycle + 1, INVALIDATION});
                                        invalidated_others = true;
                                    }
                                }
                            }
                            if (invalidated_others) {
                                st[c].invalidations++;
                            }
                            
                            // Set this core's line to M state in the next cycle
                            planned_changes.push_back({c, set, idx, true, M, tag, C.use_counter++, global_cycle + 1, STATE_TRANSITION});
                        } else {
                            stall_until[c] = bus.busy_until;
                            continue;
                        }
                    }
                } else { // Read hit
                    planned_changes.push_back({c, set, idx, true,
                                               C.sets[set][idx].state, tag,
                                               C.use_counter++, global_cycle + 1, STATE_TRANSITION});
                }
                refq[c].pop_front();
                st[c].instr++;
                if (isWrite)
                    st[c].writes++;
                else
                    st[c].reads++;
            } else { // Cache miss
                if (!bus.free_at(global_cycle)) {
                    stall_until[c] = bus.busy_until;
                    continue;
                }

                st[c].waiting_for_own_request = true;

                for (int other_c = 0; other_c < 4; other_c++) {
                    if (other_c != c) {
                        st[other_c].waiting_for_own_request = false;
                    }
                }
                
                st[c].misses++;
                bool isRdX = isWrite;
                bool found_shared = false, found_mod = false;

                vector<pair<int, int>> other_copies;
                
                for (int o = 0; o < 4; o++) {
                    if (o != c) {
                        int oi = cache[o].find_line(tag, set);
                        if (oi >= 0 && cache[o].sets[set][oi].state != I) {
                            found_shared = true;
                            other_copies.push_back({o, oi});
                            
                            if (cache[o].sets[set][oi].state == M) {
                                found_mod = true;
                            }
                        }
                    }
                }
                
                // Now check planned changes to see if any core is about to update this block
                for (const auto& pc : planned_changes) {
                    if (pc.apply_cycle > global_cycle && pc.core != c && 
                        pc.set == set && pc.tag == tag && pc.valid && pc.state != I) {
                        found_shared = true;
                        
                        // Check if this core already has the block (to avoid duplicates)
                        bool already_counted = false;
                        for (const auto& other : other_copies) {
                            if (other.first == pc.core && other.second == pc.idx) {
                                already_counted = true;
                                break;
                            }
                        }
                        
                        if (!already_counted) {
                            other_copies.push_back({pc.core, pc.idx});
                        }
                        
                        if (pc.state == M) {
                            found_mod = true;
                        }
                    }
                }

                State new_state;
                unsigned long long data_transfer_cycles;
                bool needs_invalidation = false;
                
                if (isRdX) { // Write miss
                    new_state = State::M; 
                    
                    if (found_mod) {
                        data_transfer_cycles = 200;
                        
                        for (const auto& other : other_copies) {
                            int o = other.first;
                            int oi = other.second;
                            
                            if (cache[o].sets[set][oi].state == M) {
                                stall_requests.push_back({o, global_cycle + 101});
                                st[o].traffic += (1u << b);
                            }
                            
                            needs_invalidation = true;
                        }
                    } else {
                        data_transfer_cycles = 101;
                        st[c].traffic += (1u << b);
                        
                        if (!other_copies.empty()) {
                            needs_invalidation = true;
                        }
                    }
                } else { // Read miss
                    if (found_shared) {
                        new_state = S;
                        data_transfer_cycles = 2 * block_words;
                        bool data_transferred = false;
                        for (const auto& other : other_copies) {
                            int o = other.first;
                            int oi = other.second;
                            
                            // Skip if this line is already scheduled for invalidation
                            bool skip = false;
                            for (const auto& pc : planned_changes) {
                                if (pc.core == o && pc.set == set && pc.idx == oi && pc.state == I) {
                                    skip = true;
                                    break;
                                }
                            }
                            
                            if (skip) continue;
                            
                            // Handle data transfer from the first valid core
                            if (!data_transferred && cache[o].sets[set][oi].state != I) {
                                st[o].traffic += (1u << b);  // Data transfer traffic
                                
                                // Check if providing core has M state and handle writeback
                                if (cache[o].sets[set][oi].state == M) {
                                    st[o].traffic += (1u << b);  // Additional writeback traffic
                                    
                                    // Stall providing core for cache-to-cache transfer + writeback to memory
                                    stall_requests.push_back({o, global_cycle + 2 * block_words + 100});
                                } else {
                                    // For non-modified states, just stall for the transfer time
                                    stall_requests.push_back({o, global_cycle + 2 * block_words});
                                }
                                
                                data_transferred = true;
                            }
                            
                            planned_changes.push_back(
                                {o, set, oi, true, S,
                                 cache[o].sets[set][oi].tag,
                                 cache[o].sets[set][oi].last_used,
                                 global_cycle + 1, STATE_TRANSITION});
                        }
                        data_transfer_cycles = 2 * block_words;
                    } else {
                        new_state = State::E;
                        data_transfer_cycles = 101;  // 100 cycles for memory access + 1 cycle for state transition
                        st[c].traffic += (1u << b);
                    }
                }

                unsigned long long total_bus_cycles = data_transfer_cycles;
                
                if (needs_invalidation) {
                    for (const auto& other : other_copies) {
                        int o = other.first;
                        int oi = other.second;
                        planned_changes.push_back({o, set, oi, false, I, cache[o].sets[set][oi].tag,  0, global_cycle + 1, INVALIDATION});
                    }
                    st[c].invalidations++;
                }
                
                int v = C.choose_victim(set);
                bool needs_writeback = false;
                
                if (C.sets[set][v].valid) {
                    if (C.sets[set][v].state == M) {
                        needs_writeback = true;
                        st[c].writebacks++;
                        st[c].traffic += (1u << b);
                        total_bus_cycles += 100; // Additional 100 cycles for writeback
                    }
                    if (C.sets[set][v].state != I)
                        st[c].evictions++;
                }

                unsigned long long allocation_completion_cycle = global_cycle + total_bus_cycles;
                PendingAllocation pa;
                pa.core = c;
                pa.set = set;
                pa.victim = v;
                pa.tag = tag;
                pa.state = new_state;
                pa.complete_cycle = allocation_completion_cycle;
                
                pending_allocations.push_back(pa);
                
                bus.occupy(global_cycle, total_bus_cycles);
                stall_until[c] = bus.busy_until;
            }
        }

        for (const auto& req : stall_requests) {
            if (global_cycle >= stall_until[req.core]) {
                stall_until[req.core] = req.until_cycle;
            } else {
                stall_until[req.core] = max(stall_until[req.core], stall_until[req.core] + (req.until_cycle - global_cycle));
            }
        }
        stall_requests.clear();

        global_cycle++;
    }

    auto end_time = chrono::high_resolution_clock::now();
    chrono::duration<double> elapsed = end_time - start_time;
    ostream* out = &cout;
    if (!outfn.empty()) {
        static ofstream f(outfn);
        if (!f) {
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
    *out << "Cache Size (KB per core): " << ((1 << s) * E * (1 << b) / 1024)
         << "\n";
    *out << "MESI Protocol: Enabled\n";
    *out << "Write Policy: Write-back, Write-allocate\n";
    *out << "Replacement Policy: LRU\n";
    *out << "Bus: Central snooping bus\n\n";

    unsigned long long total_bus_tx = 0, total_bus_traffic = 0;
    for (int c = 0; c < 4; c++) {
        double miss_rate =
            st[c].misses / double(st[c].instr) * 100.0;
        *out << "Core " << c << " Statistics:\n";
        *out << "Total Instructions: " << st[c].instr << "\n";
        *out << "Total Reads: " << st[c].reads << "\n";
        *out << "Total Writes: " << st[c].writes << "\n";
        *out << "Total Execution Cycles: " << st[c].execution_cycles << "\n";
        *out << "Idle Cycles: " << st[c].idle << "\n";
        *out << "Cache Misses: " << st[c].misses << "\n";
        *out << fixed << setprecision(2) << "Cache Miss Rate: " << miss_rate
             << "%\n";
        *out << "Cache Evictions: " << st[c].evictions << "\n";
        *out << "Writebacks: " << st[c].writebacks << "\n";
        *out << "Bus Invalidations: " << st[c].invalidations << "\n";
        *out << "Data Traffic (Bytes): " << st[c].traffic << "\n\n";
        total_bus_tx += st[c].invalidations;
        total_bus_traffic += st[c].traffic;
    }

    *out << "Overall Bus Summary:\n";
    *out << "Total Bus Transactions: " << total_bus_tx << "\n";
    *out << "Total Bus Traffic (Bytes): " << total_bus_traffic << "\n";
    *out << "Simulation Run Time (seconds): " << fixed << setprecision(6) << elapsed.count() << "\n";
    *out << "Total Cycles: " << global_cycle << "\n";
    return 0;
}
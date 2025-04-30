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
    uint64_t instr = 0, reads = 0, writes = 0;
    uint64_t execution_cycles = 0, idle = 0;
    uint64_t misses = 0, evictions = 0, writebacks = 0;
    uint64_t invalidations = 0, traffic = 0;
    bool waiting_for_own_request = false;
};

struct Ref {
    char type;
    uint32_t addr;
};

// Function to format address for debug output
string format_addr(uint32_t addr, int s, int b) {
    stringstream ss;
    ss << "0x" << hex << addr << " (tag:0x" << (addr >> (s + b))
       << ", set:" << dec << ((addr >> b) & ((1u << s) - 1))
       << ", offset:" << (addr & ((1u << b) - 1)) << ")";
    return ss.str();
}

int main(int argc, char* argv[]) {
    vector<string> traces(4);
    string pref;
    int s = 0, E = 0, b = 0;
    string outfn;
    bool debug_mode = false;  // Flag for detailed debugging

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
        else if (a == "-d")
            debug_mode = true;
        else if (a == "-h") {
            cout << "-t <tracefile>: name of parallel application\n";
            cout << "-s <s>: number of set index bits\n";
            cout << "-E <E>: associativity\n";
            cout << "-b <b>: number of block bits\n";
            cout << "-o <outfilename>: logs output\n";
            cout << "-d: enable detailed debugging\n";
            cout << "-h: prints this help\n";
            return 0;
        }
    }

    if (debug_mode) {
        cout << "===== SIMULATION SETUP =====\n";
        cout << "Trace prefix: " << pref << "\n";
        cout << "Set index bits (s): " << s << "\n";
        cout << "Associativity (E): " << E << "\n";
        cout << "Block bits (b): " << b << "\n";
        cout << "Output file: " << (outfn.empty() ? "stdout" : outfn) << "\n";
        cout << "===========================\n\n";
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
            uint32_t a = stoul(addr, nullptr, 0);
            refq[c].push_back({t, a});
        }

        if (debug_mode) {
            cout << "Core " << c << " loaded " << refq[c].size() << " memory references\n";
        }
    }

    vector<Cache> cache(4, Cache(s, E, b));
    Bus bus;
    vector<Stats> st(4);
    vector<uint64_t> stall_until(4, 0);
    uint64_t global_cycle = 0;
    vector<PendingAllocation> pending_allocations;
    vector<PlannedChange> planned_changes;

    auto get_set = [&](uint32_t addr) { return (addr >> b) & ((1u << s) - 1); };
    auto get_tag = [&](uint32_t addr) { return addr >> (s + b); };
    auto block_words = (1u << b) / 4u;

    if (debug_mode) {
        cout << "\n===== SIMULATION START =====\n";
    }

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

        if (debug_mode) {
            cout << "\n==== CYCLE " << global_cycle << " ====\n";
            
            // Debug stall status
            cout << "Stall status: ";
            for (int c = 0; c < 4; c++) {
                cout << "Core " << c << ":" << (global_cycle < stall_until[c] ? "STALLED" : "ACTIVE");
                if (global_cycle < stall_until[c]) {
                    cout << " until " << stall_until[c];
                }
                cout << " | ";
            }
            cout << endl;
            
            // Debug bus status
            cout << "Bus status: ";
            if (bus.free_at(global_cycle)) {
                cout << "FREE";
            } else {
                cout << "BUSY until " << bus.busy_until;
            }
            cout << endl;
            
            // Debug pending allocations count
            cout << "Pending allocations: " << pending_allocations.size() << endl;
            cout << "Planned changes: " << planned_changes.size() << endl;
        }

        // First, apply any changes scheduled for this cycle
        // First, apply any changes scheduled for this cycle
        vector<PlannedChange> next_planned_changes;
        for (auto& pc : planned_changes) {
            if (pc.apply_cycle <= global_cycle) {
                if (pc.type == STATE_TRANSITION) {
                    Cache& C = cache[pc.core];
                    if (debug_mode) {
                        cout << "APPLYING STATE TRANSITION: Core " << pc.core 
                            << " Set " << pc.set << " Line " << pc.idx 
                            << " State " << StateNames.at(C.sets[pc.set][pc.idx].state)
                            << " -> " << StateNames.at(pc.state) << endl;
                    }
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
                if (debug_mode) {
                    cout << "APPLYING INVALIDATION: Core " << pc.core 
                        << " Set " << pc.set << " Line " << pc.idx 
                        << " State " << StateNames.at(C.sets[pc.set][pc.idx].state)
                        << " -> I" << endl;
                }
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
                if (debug_mode) {
                    cout << "ALLOCATION COMPLETE: Core " << pa.core 
                        << " Set " << pa.set << " Line " << pa.victim 
                        << " Tag 0x" << hex << pa.tag << dec 
                        << " State " << StateNames.at(pa.state) << endl;
                    cout << "Before allocation: ";
                    cache[pa.core].print_set_state(pa.core, pa.set);
                    st[pa.core].waiting_for_own_request = false;
                }

                // Apply the allocation
                Cache& C = cache[pa.core];
                C.sets[pa.set][pa.victim].valid = true;
                C.sets[pa.set][pa.victim].tag = pa.tag;
                C.sets[pa.set][pa.victim].state = pa.state;
                C.touch(pa.set, pa.victim);

                if (debug_mode) {
                    cout << "After allocation: ";
                    cache[pa.core].print_set_state(pa.core, pa.set);
                }
            } else {
                if (debug_mode) {
                    cout << "ALLOCATION PENDING: Core " << pa.core 
                        << " Set " << pa.set << " Line " << pa.victim 
                        << " Tag 0x" << hex << pa.tag << dec 
                        << " State " << StateNames.at(pa.state) 
                        << " Complete at cycle " << pa.complete_cycle << endl;
                }
                pending_allocations.push_back(pa);
            }
        }

        // Process memory references for each core
        for (int c = 0; c < 4; c++) {
            if (refq[c].empty()) continue;
            if (global_cycle < stall_until[c]) {
                if (!st[c].waiting_for_own_request) {
                    st[c].idle++;
                    if (debug_mode) {
                        cout << "Core " << c << " is stalled due to another core's request until cycle " 
                             << stall_until[c] << " (IDLE)" << endl;
                        cout << "Idle Cycle Count for Core " << c << " is " << st[c].idle << endl;
                    }
                } else {
                    st[c].execution_cycles++;
                    if (debug_mode) {
                        cout << "Core " << c << " is waiting for its own request until cycle " 
                             << stall_until[c] << " (EXECUTION)" << endl;
                        cout << "Execution Cycle Count for Core " << c << " is " << st[c].execution_cycles << endl;
                    }
                }
                continue;
            }

            st[c].execution_cycles++;
            
            Ref R = refq[c].front();
            uint32_t set = get_set(R.addr), tag = get_tag(R.addr);
            bool isWrite = (R.type == 'W');
            Cache& C = cache[c];
            int idx = C.find_line(tag, set);

            if (debug_mode) {
                cout << "\nCORE " << c << " -> " << (isWrite ? "WRITE" : "READ") << " " 
                     << format_addr(R.addr, s, b) << endl;
                cout << "Cache lookup: Set " << set << ", Tag 0x" << hex << tag << dec 
                     << ", Result: " << (idx >= 0 ? "HIT at index " + to_string(idx) : "MISS") << endl;
                if (idx >= 0) {
                    cout << "Line state: " << StateNames.at(C.sets[set][idx].state) << endl;
                }
                C.print_set_state(c, set);
            }

            if (idx >= 0 && C.sets[set][idx].state != I) {
                if (isWrite) {
                    if (debug_mode) {
                        cout << "WRITE HIT in Core " << c << " at cycle " << global_cycle 
                             << " for instr " << st[c].instr << endl;
                    }
                    
                    if (C.sets[set][idx].state == M) {
                        if (debug_mode) {
                            cout << "State transition: M -> M (already modified)" << endl;
                        }
                        planned_changes.push_back(
                            {c, set, idx, true, M, tag, C.use_counter++, global_cycle + 1, STATE_TRANSITION});
                    } else if (C.sets[set][idx].state == E) {
                        if (debug_mode) {
                            cout << "State transition: E -> M (modifying exclusive line)" << endl;
                        }
                        planned_changes.push_back(
                            {c, set, idx, true, M, tag, C.use_counter++, global_cycle + 1, STATE_TRANSITION});
                    } else if (C.sets[set][idx].state == S) {
                        if (bus.free_at(global_cycle)) {
                            if (debug_mode) {
                                cout << "State transition: S -> M (modifying shared line)" << endl;
                                cout << "Need to invalidate other copies..." << endl;
                            }
                            bool invalidated_others = false;
                            
                            for (int o = 0; o < 4; o++) {
                                if (o != c) {
                                    int oi = cache[o].find_line(tag, set);
                                    if (oi >= 0 && cache[o].sets[set][oi].state != I) {
                                        if (debug_mode) {
                                            cout << "Will invalidate copy in Core " << o << " index " << oi << endl;
                                        }
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
                            if (debug_mode) {
                                cout << "Bus busy, stalling Core " << c << " until cycle " 
                                     << bus.busy_until << endl;
                            }
                            stall_until[c] = bus.busy_until;
                            continue;
                        }
                    }
                } else { // Read hit
                    if (debug_mode) {
                        cout << "READ HIT in Core " << c << " at cycle " << global_cycle 
                             << " for instr " << st[c].instr << endl;
                        cout << "State remains " << StateNames.at(C.sets[set][idx].state) << endl;
                    }
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
                    if (debug_mode) {
                        cout << "Bus busy, stalling Core " << c << " until cycle " 
                             << bus.busy_until << endl;
                    }
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
                
                if (debug_mode) {
                    cout << "CACHE MISS in Core " << c << " at cycle " << global_cycle 
                         << " for instr " << st[c].instr << endl;
                    cout << "Checking other cores for same block..." << endl;
                }
                
                // Cache miss - check ALL cores (including those that just updated this cycle)
                vector<pair<int, int>> other_copies;
                
                for (int o = 0; o < 4; o++) {
                    if (o != c) {
                        int oi = cache[o].find_line(tag, set);
                        if (oi >= 0 && cache[o].sets[set][oi].state != I) {
                            found_shared = true;
                            other_copies.push_back({o, oi});
                            
                            if (cache[o].sets[set][oi].state == M) {
                                found_mod = true;
                                if (debug_mode) {
                                    cout << "Core " << o << " has this block in M state" << endl;
                                }
                            } else if (debug_mode) {
                                cout << "Core " << o << " has this block in " 
                                     << StateNames.at(cache[o].sets[set][oi].state) << " state" << endl;
                            }
                        }
                    }
                }
                
                // Now check planned changes to see if any core is about to update this block
                for (const auto& pc : planned_changes) {
                    if (pc.apply_cycle > global_cycle && pc.core != c && 
                        pc.set == set && pc.tag == tag && pc.valid && pc.state != I) {
                        
                        if (debug_mode) {
                            cout << "Core " << pc.core << " is about to update this block to " 
                                 << StateNames.at(pc.state) << " state" << endl;
                        }
                        
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
                uint64_t data_transfer_cycles;
                bool needs_invalidation = false;
                
                if (isRdX) { // Write miss
                    if (debug_mode) {
                        cout << "Processing WRITE MISS" << endl;
                    }
                    new_state = State::E;  // Corrected: Write miss goes directly to M state
                    
                    if (found_mod) {
                        if (debug_mode) {
                            cout << "Modified copy found in another core, requiring writeback" << endl;
                            cout << "Data transfer duration: 200 cycles" << endl;
                        }
                        data_transfer_cycles = 200;
                        
                        for (const auto& other : other_copies) {
                            int o = other.first;
                            int oi = other.second;
                            
                            if (cache[o].sets[set][oi].state == M) {
                                stall_requests.push_back({o, global_cycle + 101});
                                if (debug_mode) {
                                    cout << "Core " << o << " writes back modified data (" 
                                         << (1u << b) << " bytes)" << endl;
                                    cout << "Will invalidate copy in Core " << o << endl;
                                    cout << "Adding stall request for Core " << o << " until cycle " << (global_cycle + 101)<< endl;
                                }
                                st[o].traffic += (1u << b);
                            } else if (debug_mode) {
                                cout << "Will invalidate non-modified copy in Core " << o << endl;
                            }
                            
                            needs_invalidation = true;
                        }
                    } else {
                        if (debug_mode) {
                            cout << "No modified copy found, fetching from memory" << endl;
                            cout << "Data transfer duration: 100 cycles" << endl;
                        }
                        data_transfer_cycles = 101;
                        st[c].traffic += (1u << b);
                        
                        if (!other_copies.empty()) {
                            needs_invalidation = true;
                            if (debug_mode) {
                                cout << "Will invalidate " << other_copies.size() << " non-modified copies" << endl;
                            }
                        }
                    }
                } else { // Read miss
                    if (debug_mode) {
                        cout << "Processing READ MISS" << endl;
                    }
                    
                    if (found_shared) {
                        new_state = S;
                        data_transfer_cycles = 2 * block_words;
                        if (debug_mode) {
                            cout << "Valid copy found in another core" << endl;
                            cout << "Data transfer duration: " << data_transfer_cycles << " cycles" << endl;
                            cout << "New state will be S (Shared)" << endl;
                        }
                        
                        bool data_transferred = false;
                        for (const auto& other : other_copies) {
                            int o = other.first;
                            int oi = other.second;
                            
                            if (!data_transferred) {
                                if (debug_mode) {
                                    cout << "Core " << o << " provides data (" 
                                         << (1u << b) << " bytes)" << endl;
                                }
                                st[o].traffic += (1u << b);
                                data_transferred = true;
                            }
                            
                            // Update other cores' copies to S state
                            if (debug_mode) {
                                cout << "Changing state of copy in Core " << o 
                                     << " to S (if not already)" << endl;
                            }
                            
                            // Skip applying S state if the core is already pending invalidation
                            bool skip = false;
                            for (const auto& pc : planned_changes) {
                                if (pc.core == o && pc.set == set && pc.idx == oi && pc.state == I) {
                                    skip = true;
                                    break;
                                }
                            }
                            
                            if (!skip) {
                                // Add stall for other core providing data
                                if (!data_transferred && cache[o].sets[set][oi].state != I) {
                                    stall_requests.push_back({o, global_cycle + 2 * block_words});
                                    if (debug_mode) {
                                        cout << "Adding stall request for Core " << o << " until cycle " 
                                             << (global_cycle + 2 * block_words) << endl;
                                    }
                                    data_transferred = true;
                                }
                                
                                planned_changes.push_back(
                                    {o, set, oi, true, S,
                                     cache[o].sets[set][oi].tag,
                                     cache[o].sets[set][oi].last_used,
                                     global_cycle + 1, STATE_TRANSITION});
                            }
                            
                        }
                    } else {
                        new_state = State::E;
                        data_transfer_cycles = 101;
                        st[c].traffic += (1u << b);
                        if (debug_mode) {
                            cout << "No valid copy found, fetching from memory" << endl;
                            cout << "Data transfer duration: " << data_transfer_cycles << " cycles" << endl;
                            cout << "New state will be E (Exclusive)" << endl;
                        }
                    }
                }

                // Calculate total bus time
                // uint64_t invalidation_cycles = needs_invalidation ? 1 : 0;
                uint64_t total_bus_cycles = data_transfer_cycles;
                
                if (needs_invalidation) {
                    for (const auto& other : other_copies) {
                        int o = other.first;
                        int oi = other.second;
                        if (debug_mode) {
                            cout << "Scheduling invalidation for Core " << o << " at cycle " 
                                 << (global_cycle + 1) << endl;
                        }
                        planned_changes.push_back({o, set, oi, false, I, cache[o].sets[set][oi].tag,  0, global_cycle + 1, INVALIDATION});
                    }
                    st[c].invalidations++;
                }
                
                int v = C.choose_victim(set);
                bool needs_writeback = false;
                
                if (debug_mode) {
                    cout << "Victim selection: Line " << v;
                    if (C.sets[set][v].valid) {
                        cout << " (valid, tag 0x" << hex << C.sets[set][v].tag << dec 
                             << ", state " << StateNames.at(C.sets[set][v].state) << ")";
                    } else {
                        cout << " (invalid)";
                    }
                    cout << endl;
                }
                
                if (C.sets[set][v].valid) {
                    if (C.sets[set][v].state == M) {
                        needs_writeback = true;
                        st[c].writebacks++;
                        st[c].traffic += (1u << b);
                        total_bus_cycles += 100; // Additional 100 cycles for writeback
                        if (debug_mode) {
                            cout << "Victim is in M state, requires writeback" << endl;
                            cout << "Writeback traffic: " << (1u << b) << " bytes" << endl;
                            cout << "Additional writeback time: 100 cycles" << endl;
                        }
                    }
                    if (C.sets[set][v].state != I)
                        st[c].evictions++;
                }

                uint64_t allocation_completion_cycle = global_cycle + total_bus_cycles;
                
                if (debug_mode) {
                    cout << "Allocation will complete at cycle: " << allocation_completion_cycle << endl;
                    cout << "Total bus occupation: " << total_bus_cycles << " cycles" << endl;
                }

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
                
                if (debug_mode) {
                    cout << "Core " << c << " stalled until cycle " << stall_until[c] << endl;
                }
            }
        }

        for (const auto& req : stall_requests) {
            stall_until[req.core] = max(stall_until[req.core], req.until_cycle);
            if (debug_mode) {
                cout << "Applied stall request: Core " << req.core << " stalled until cycle " 
                     << req.until_cycle << endl;
            }
        }
        stall_requests.clear();

        global_cycle++;
    }

    auto end_time = chrono::high_resolution_clock::now();
    chrono::duration<double> elapsed = end_time - start_time;

    if (debug_mode) {
        cout << "\n===== SIMULATION COMPLETED =====\n";
        cout << "Total cycles: " << global_cycle << endl;
    }

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

    uint64_t total_bus_tx = 0, total_bus_traffic = 0;
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

    return 0;
}

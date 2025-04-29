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

// Transaction types
const int WRITE_BACK = 0;
const int BUS_RD = 1;
const int BUS_RDX = 2;

// Cache states
const char INVALID = 'I';
const char SHARED = 'S';
const char EXCLUSIVE = 'E';
const char MODIFIED = 'M';

// Structs and Classes

struct CacheLine {
    uint32_t tag;
    char state;
    CacheLine() : tag(0), state(INVALID) {}
};

struct Set {
    vector<CacheLine> lines;
    list<int> lru_order; // front is MRU, back is LRU
    Set(int E) : lines(E) {
        for (int i = 0; i < E; i++) lru_order.push_back(i);
    }
};

class Cache {
public:
    Cache(int s, int E, int b, int core_id) 
        : s(s), E(E), b(b), core_id(core_id), S(1 << s), B(1 << b), sets(S, Set(E)),
          num_accesses(0), num_misses(0), num_evictions(0), num_writebacks(0), num_invalidations(0) {}

    char check_hit(uint32_t address, int& line_index) {
        uint32_t set_index = get_set_index(address);
        uint32_t tag = get_tag(address);
        Set& set = sets[set_index];
        for (int i = 0; i < E; i++) {
            if (set.lines[i].state != INVALID && set.lines[i].tag == tag) {
                line_index = i;
                return set.lines[i].state;
            }
        }
        return INVALID;
    }

    void update_lru(uint32_t set_index, int line_index) {
        Set& set = sets[set_index];
        set.lru_order.remove(line_index);
        set.lru_order.push_front(line_index);
    }

    int choose_victim(uint32_t set_index) {
        Set& set = sets[set_index];
        return set.lru_order.back();
    }

    void install_block(uint32_t address, char state, uint32_t set_index, int line_index) {
        uint32_t tag = get_tag(address);
        Set& set = sets[set_index];
        set.lines[line_index].tag = tag;
        set.lines[line_index].state = state;
        update_lru(set_index, line_index);
    }

    bool has_block(uint32_t address, char& state) {
        uint32_t set_index = get_set_index(address);
        uint32_t tag = get_tag(address);
        Set& set = sets[set_index];
        for (const auto& line : set.lines) {
            if (line.state != INVALID && line.tag == tag) {
                state = line.state;
                return true;
            }
        }
        return false;
    }

    void snoop(int transaction_type, uint32_t address) {
        uint32_t set_index = get_set_index(address);
        uint32_t tag = get_tag(address);
        Set& set = sets[set_index];
        for (int i = 0; i < E; i++) {
            if (set.lines[i].tag == tag && set.lines[i].state != INVALID) {
                if (transaction_type == BUS_RD) {
                    if (set.lines[i].state == EXCLUSIVE) {
                        set.lines[i].state = SHARED;
                    }
                    // 'M' is handled when provider is determined
                } else if (transaction_type == BUS_RDX) {
                    if (set.lines[i].state != INVALID) {
                        set.lines[i].state = INVALID;
                        num_invalidations++;
                    }
                }
                break;
            }
        }
    }

    // Public accessor for set_index
    uint32_t get_set_index(uint32_t address) const {
        uint32_t mask = (1 << s) - 1;
        return (address >> b) & mask;
    }

    // Public method to check if a line is invalid
    bool is_line_invalid(uint32_t set_index, int line_index) const {
        return sets[set_index].lines[line_index].state == INVALID;
    }

    // Public method to get victim state
    char get_line_state(uint32_t set_index, int line_index) const {
        return sets[set_index].lines[line_index].state;
    }

    // Public method to get victim tag
    uint32_t get_line_tag(uint32_t set_index, int line_index) const {
        return sets[set_index].lines[line_index].tag;
    }

    int num_accesses, num_misses, num_evictions, num_writebacks, num_invalidations;

private:
    int s, E, b, core_id;
    int S, B;
    vector<Set> sets;

    uint32_t get_tag(uint32_t address) {
        return address >> (b + s);
    }
};

struct BusTransaction {
    int type;
    int core;
    uint32_t address;
    int provider; // -1 for memory
    int cycles;
};

class Bus {
public:
    Bus(int N) : N(N), busy(false), cycles_left(0) {}

    void initiate_transaction(int type, int core, uint32_t address, int provider) {
        busy = true;
        current_transaction = {type, core, address, provider, 0};
        if (type == WRITE_BACK) {
            cycles_left = 100;
        } else if (type == BUS_RD || type == BUS_RDX) {
            cycles_left = provider != -1 ? 2 * N : 100;
        }
    }

    void tick() {
        if (busy) {
            cycles_left--;
            if (cycles_left <= 0) busy = false;
        }
    }

    bool is_busy() { return busy; }
    int get_cycles_left() { return cycles_left; }
    BusTransaction get_current_transaction() { return current_transaction; }

private:
    int N;
    bool busy;
    int cycles_left;
    BusTransaction current_transaction;
};

class Simulator {
public:
    Simulator(int s, int E, int b, string trace_prefix, string output_file = "")
        : s(s), E(E), b(b), cycle(0), bus(B / 4), instruction_indices(4, 0), finished(4, false),
          core_states(4, IDLE), num_reads(4, 0), num_writes(4, 0), execution_cycles(4, 0),
          idle_cycles(4, 0), data_traffic(0) {
        caches = vector<Cache>(4, Cache(0, 0, 0, 0));
        for (int i = 0; i < 4; i++) {
            caches[i] = Cache(s, E, b, i);
            string filename = trace_prefix + "_proc" + to_string(i) + ".trace";
            instructions.push_back(read_trace(filename));
        }
        core_infos.resize(4);
        if (!output_file.empty()) {
            ofs.open(output_file);
            if (!ofs.is_open()) {
                cerr << "Error opening output file " << output_file << endl;
                exit(1);
            }
        }
    }

    ~Simulator() {
        if (ofs.is_open()) ofs.close();
    }


    void run() {
        while (!all_finished()) {
            cycle++;
    
            // Process bus
            if (bus.is_busy()) {
                bus.tick();
                if (!bus.is_busy()) {
                    BusTransaction trans = bus.get_current_transaction();
                    int core = trans.core;
                    uint32_t addr = trans.address;
                    if (trans.type == WRITE_BACK) {
                        core_states[core] = WAITING_FOR_FETCH;
                        bus_requests.push({core, core_infos[core].is_write ? BUS_RDX : BUS_RD, addr});
                    } else if (trans.type == BUS_RD) {
                        char state = trans.provider != -1 ? SHARED : EXCLUSIVE;
                        caches[core].install_block(addr, state, core_infos[core].set_index, core_infos[core].line_index);
                        if (core_infos[core].is_write) {
                            caches[core].install_block(addr, MODIFIED, core_infos[core].set_index, core_infos[core].line_index);
                        }
                        core_states[core] = IDLE;
                        instruction_indices[core]++;
                    } else if (trans.type == BUS_RDX) {
                        caches[core].install_block(addr, EXCLUSIVE, core_infos[core].set_index, core_infos[core].line_index);
                        if (core_infos[core].is_write) {
                            caches[core].install_block(addr, MODIFIED, core_infos[core].set_index, core_infos[core].line_index);
                        }
                        core_states[core] = IDLE;
                        instruction_indices[core]++;
                    }
                    data_traffic += B;
                }
            }
    
            // Process pending bus requests
            if (!bus.is_busy() && !bus_requests.empty()) {
                BusRequest req = bus_requests.front();
                bus_requests.pop();
                int provider = -1;
                bool has_modified = false;
                for (int i = 0; i < 4; i++) {
                    char state;
                    if (caches[i].has_block(req.address, state)) {
                        if (state == MODIFIED) {
                            provider = i;
                            caches[i].snoop(req.type, req.address);
                            has_modified = true;
                            break;
                        }
                    }
                }
                if (!has_modified) {
                    for (int i = 0; i < 4; i++) {
                        char state;
                        if (caches[i].has_block(req.address, state) && state != INVALID) {
                            provider = i;
                            break;
                        }
                    }
                }
                for (int i = 0; i < 4; i++) {
                    if (i != req.core) caches[i].snoop(req.type, req.address);
                }
                bus.initiate_transaction(req.type, req.core, req.address, provider);
                core_states[req.core] = (req.type == WRITE_BACK) ? WAITING_FOR_WRITE_BACK : WAITING_FOR_FETCH;
            }
    
            // Process each core
            for (int core = 0; core < 4; core++) {
                if (finished[core] || core_states[core] != IDLE) {
                    if (!finished[core]) idle_cycles[core]++;
                    continue;
                }
    
                if (instruction_indices[core] >= instructions[core].size()) {
                    finished[core] = true;
                    execution_cycles[core] = cycle;
                    continue;
                }
    
                auto [op, addr] = instructions[core][instruction_indices[core]];
                bool is_write = (op == 'W');
                if (is_write) num_writes[core]++;
                else num_reads[core]++;
                caches[core].num_accesses++;
    
                int line_index;
                char state = caches[core].check_hit(addr, line_index);
                uint32_t set_index = caches[core].get_set_index(addr);
    
                if (state != INVALID) {
                    // Hit
                    if (is_write) {
                        if (state == EXCLUSIVE) {
                            caches[core].install_block(addr, MODIFIED, set_index, line_index);
                        } else if (state == SHARED) {
                            core_states[core] = WAITING_FOR_FETCH;
                            core_infos[core] = {core_states[core], addr, is_write, set_index, line_index};
                            bus_requests.push({core, BUS_RDX, addr});
                        }
                    }
                    caches[core].update_lru(set_index, line_index);
                    instruction_indices[core]++;
                } else {
                    // Miss
                    caches[core].num_misses++;
                    bool has_invalid = false;
                    for (int i = 0; i < E; i++) {
                        if (caches[core].is_line_invalid(set_index, i)) {
                            line_index = i;
                            has_invalid = true;
                            break;
                        }
                    }
                    if (!has_invalid) {
                        line_index = caches[core].choose_victim(set_index);
                        char victim_state = caches[core].get_line_state(set_index, line_index);
                        if (victim_state != INVALID) {
                            caches[core].num_evictions++;
                            if (victim_state == MODIFIED) {
                                caches[core].num_writebacks++;
                                core_states[core] = WAITING_FOR_WRITE_BACK;
                                core_infos[core] = {core_states[core], addr, is_write, set_index, line_index};
                                bus_requests.push({core, WRITE_BACK, caches[core].get_line_tag(set_index, line_index) << (s + b)});
                                continue;
                            }
                        }
                    }
                    core_states[core] = WAITING_FOR_FETCH;
                    core_infos[core] = {core_states[core], addr, is_write, set_index, line_index};
                    bus_requests.push({core, is_write ? BUS_RDX : BUS_RD, addr});
                }
            }
        }
    
        // Output statistics
        output_statistics();
    }

private:
    int s, E, b, B = 1 << b;
    int cycle;
    vector<vector<pair<char, uint32_t>>> instructions;
    vector<int> instruction_indices;
    vector<bool> finished;
    vector<Cache> caches;
    Bus bus;
    struct BusRequest {
        int core, type;
        uint32_t address;
    };
    queue<BusRequest> bus_requests;
    enum CoreState { IDLE, WAITING_FOR_WRITE_BACK, WAITING_FOR_FETCH };
    vector<CoreState> core_states;
    struct CoreInfo {
        int state;
        uint32_t address;
        bool is_write;
        uint32_t set_index;
        int line_index;
    };
    vector<CoreInfo> core_infos;
    vector<int> num_reads, num_writes, execution_cycles, idle_cycles;
    uint64_t data_traffic;
    ofstream ofs;

    vector<pair<char, uint32_t>> read_trace(string filename) {
        vector<pair<char, uint32_t>> trace;
        ifstream file(filename);
        if (!file.is_open()) {
            cerr << "Error opening " << filename << endl;
            exit(1);
        }
        string line;
        while (getline(file, line)) {
            istringstream iss(line);
            char op;
            string addr_str;
            iss >> op >> addr_str;
            uint32_t addr = stoul(addr_str, nullptr, 16);
            trace.emplace_back(op, addr);
        }
        file.close();
        return trace;
    }

    bool all_finished() {
        return all_of(finished.begin(), finished.end(), [](bool f) { return f; });
    }

    void output_statistics() {
        ostream& out = ofs.is_open() ? ofs : cout;
        for (int i = 0; i < 4; i++) {
            out << "Core " << i << ":\n";
            out << "Reads: " << num_reads[i] << ", Writes: " << num_writes[i] << "\n";
            out << "Execution Cycles: " << execution_cycles[i] << "\n";
            out << "Idle Cycles: " << idle_cycles[i] << "\n";
            float miss_rate = caches[i].num_accesses ? static_cast<float>(caches[i].num_misses) / caches[i].num_accesses : 0;
            out << "Miss Rate: " << fixed << setprecision(4) << miss_rate << "\n";
            out << "Evictions: " << caches[i].num_evictions << "\n";
            out << "Writebacks: " << caches[i].num_writebacks << "\n";
            out << "Invalidations: " << caches[i].num_invalidations << "\n";
        }
        out << "Total Data Traffic (bytes): " << data_traffic << "\n";
    }
};

int main(int argc, char* argv[]) {
    if (argc < 5) {
        cerr << "Usage: " << argv[0] << " <trace_prefix> <s> <E> <b> [-o output_file]\n";
        return 1;
    }

    string trace_prefix = argv[1];
    int s = atoi(argv[2]);
    int E = atoi(argv[3]);
    int b = atoi(argv[4]);
    string output_file;
    if (argc > 6 && string(argv[5]) == "-o") output_file = argv[6];

    Simulator sim(s, E, b, trace_prefix, output_file);
    sim.run();
    return 0;
}
# L1 Cache Simulator

This project simulates a Level 1 cache based on user-provided parameters and a memory access trace file.

## Compilation

To compile the code, simply run:

```bash
make
```

## Usage

After compilation, run the simulator with the following syntax:

```bash
./L1simulate -t <tracefile> -s <s> -E <E> -b <b> -o <outfilename>
```

### Parameters

- `-t <tracefile>`: Path to the memory trace file.
- `-s <s>`: Number of set index bits (cache will have 2^s sets).
- `-E <E>`: Number of lines per set (associativity).
- `-b <b>`: Number of block offset bits.
- `-o <outfilename>`: Name of the output file to write results.

## Example

```bash
./L1simulate -t traces/trace1.txt -s 4 -E 2 -b 4 -o output.txt
```

This will simulate a 2-way set associative cache with 16 sets and a block size of 16 bytes using the given trace file.

<h1 align="center">
High-Performance Distributed Word Count <br> using InfiniBand and RDMA
</h1>

---

## 📝 Abstract

<div align="justify">

This project presents a distributed and scalable solution for counting word frequencies in large text files using InfiniBand’s RDMA (Remote Direct Memory Access) capabilities. It leverages MPI (Message Passing Interface) for coordination among processes and uses a custom Distributed Hash Table (DHT) design that partitions data across processes based on a hybrid hash function.

The architecture is designed to minimize latency and maximize parallel performance by using RDMA Read and (optionally) Write operations instead of traditional message-passing for remote data access and updates. Each process is responsible for a fixed-size segment of the global hash table and can access remote segments with low-overhead one-sided RDMA operations. The implementation efficiently supports querying and updating word frequencies across nodes, demonstrating high throughput and scalability with increasing data sizes.

</div>

---

## 🛠️ Technologies Used

- C (with InfiniBand Verbs API)
- MPI (Message Passing Interface)
- RDMA (Remote Direct Memory Access)
- InfiniBand Network (libibverbs)

---

## ⚙️ Compilation and Execution

### 📦 Prerequisites

- RDMA-capable cluster or local InfiniBand environment (e.g., Mellanox)
- MPI (e.g., OpenMPI or MPICH)
- libibverbs and RDMA-core libraries

---

### 🔧 Build Instructions

1. Navigate to the project root directory and build using:

```
make
```

This compiles the `dht_count.c` file and links required RDMA and MPI libraries.

---

### 🚀 Execution Instructions

Run the distributed word count using:

```
mpirun -np 8 ./dht_count <data_file> <query_file>
```

- `<data_file>`: Input text file containing words to be inserted into the distributed hash table.
- `<query_file>`: File with query words whose counts and positions will be retrieved.

---

## 🧐 Design and Hashing Strategy

<div align="justify">

The implementation uses a fixed-size Distributed Hash Table (DHT), where each process holds 256 local buckets (each 4MB). The total key space is divided among processes using a custom 11-bit hash function:

- First 3 bits → Determine the owning process.
- Remaining 8 bits → Determine the bucket within the process.

Word normalization ensures case-insensitive storage by converting all words to uppercase before insertion or querying.

</div>

---

## 🧪 Query and Update Mechanism

<div align="justify">

- **RDMA Read**: Used to fetch word frequency and position information from remote hash tables.
- **RDMA Write (Bonus)**: If implemented, allows direct write-based updates to remote buckets, reducing context switches and synchronization overhead.
- **Atomicity**: Remote atomic operations (e.g., `IBV_WR_ATOMIC_FETCH_AND_ADD`) ensure consistency during concurrent updates.
- **Result Format**: Each query result includes the total count and up to 7 occurrence positions of the word.

</div>

---

## 📊 Performance and Complexity Analysis

<div align="justify">

The full complexity analysis and runtime profiling results are documented in `Analysis.txt`. Experiments were conducted with increasing file sizes (from 1MB to 1GB) and verified for scalability. Key observations include:

- Near-linear scalability with increasing file size.
- RDMA read throughput sustains high performance.
- Low communication overhead due to one-sided memory access.

</div>

---

## 📁 Directory Structure

```
InfiniBand/
├── Code_Examples/      # RDMA and InfiniBand setup examples
├── Examples/           # Sample data and query files
├── dht_count.c         # Main implementation of DHT + RDMA + MPI
├── Makefile            # Build instructions
├── Analysis.txt        # Complexity and performance results
└── README.md           # Project documentation
```

---

## 📊 Results, Analysis, and Complexity

<div align="justify">

For detailed performance results, complexity analysis, scalability observations, and additional experimental outputs, please feel free to contact me.

</div>

---

## 📬 Contact Information

- **Email**: manne.bharadwaj.1953@gmail.com
- **LinkedIn**: [Bharadwaj Manne](https://www.linkedin.com/in/bharadwaj-manne-711476249/)

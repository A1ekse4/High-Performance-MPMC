# Profiling and bottleneck analysis

## Environment

- OS: Linux (Fedora-based)
- Compiler: GCC 15
- Queue: bounded lock-free MPMC (Vyukov-style)

## Benchmark baseline (matrix mode)

Full matrix output was generated in `build/bench_results.csv`.

Selected rows from the matrix run:

| Producers | Consumers | Capacity | Effective ops/sec |
| --- | --- | --- | ---: |
| 1 | 1 | 262144 | 9,255,806 |
| 2 | 1 | 32768 | 13,025,853 |
| 4 | 4 | 32768 | 11,119,950 |

## Focus scenario for profiling

Command:

```bash
./build/bench_mpmc 4 4 32768 3
```

Result sample:

- enq_ops: 21,762,220
- deq_ops: 21,729,457
- effective_ops_per_sec: 7,242,409.41

## Bottleneck hypotheses

- CAS contention on global `enqueue_pos`/`dequeue_pos` under high producer/consumer counts.
- Cache-line bouncing around hot atomics when thread count grows.
- Queue occupancy swings causing producer/consumer retries and wasted cycles.

## Optimization iterations applied

1. **Baseline lock-free ring buffer**
   - Per-slot sequence counters.
   - Acquire/release synchronization around slot publication/consumption.
2. **Cache-line layout tuning**
   - `alignas(64)` for hot structures and padded atomic counters.
   - Reduced false sharing between enqueue/dequeue hot paths.
3. **CAS-failure backoff**
   - Short bounded pause loop to reduce aggressive retry pressure.
4. **Type-path tuning**
   - Skip destructor work for trivially destructible payloads.

## perf status

`perf` is not installed in the current environment (`perf: command not found`), so hardware-counter evidence is pending.

When available, run:

```bash
bash scripts/profile.sh
perf report -i build/profiling/perf.data
```

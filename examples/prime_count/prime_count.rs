// Rust prime-count benchmark - same workload as prime_count.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

const LIMIT: i64 = 50_000;
const PASSES: i32 = 200;

fn is_prime(n: i64) -> bool {
    if n < 2 {
        return false;
    }
    let mut d = 2i64;
    while d * d <= n {
        if n % d == 0 {
            return false;
        }
        d += 1;
    }
    true
}

fn count_primes(limit: i64) -> i64 {
    let mut count = 0i64;
    for n in 2..=limit {
        if is_prime(n) {
            count += 1;
        }
    }
    count
}

fn main() {
    println!("Prime count: trial division up to 50000");

    let primes = count_primes(LIMIT);
    println!("Primes = {primes}");
    println!("Benchmark: {PASSES} passes (count_primes)");

    let t0 = bench_time_us();
    let mut bench_sum = 0i64;
    for p in 0..PASSES {
        bench_sum += count_primes(LIMIT);
        black_box(p);
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Bench sum = {bench_sum}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);
}

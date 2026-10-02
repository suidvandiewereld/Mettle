// Rust Collatz benchmark - same workload as collatz.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

fn collatz_steps(mut n: i64) -> i64 {
    let mut count: i64 = 0;
    while n > 1 {
        if n % 2 == 0 {
            n /= 2;
        } else {
            n = 3 * n + 1;
        }
        count += 1;
    }
    count
}

fn main() {
    const PASSES: i32 = 10;

    println!("Collatz: sum of steps for n=1..100000");

    let mut sum: i64 = 0;
    for n in 1..=100_000 {
        sum += collatz_steps(n);
    }
    println!("Sum (1..100000) = {sum}");
    println!("Benchmark: {PASSES} passes (sum steps 1..100000 each)");

    let t0 = bench_time_us();
    let mut bench_sum: i64 = 0;
    for _ in 0..PASSES {
        for i in 1..=100_000 {
            bench_sum += collatz_steps(i);
        }
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Bench sum = {bench_sum}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);

    black_box(bench_sum);
}

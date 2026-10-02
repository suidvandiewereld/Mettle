// Rust sum-of-squares benchmark - same workload as sum_squares.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

fn sum_squares(n: i64) -> i64 {
    let mut sum: i64 = 0;
    for i in 1..=n {
        sum += i * i;
    }
    sum
}

fn main() {
    const PASSES: i32 = 200;

    println!("Sum of squares: 1²+2²+...+100000²");

    let result = sum_squares(100_000);
    println!("Sum = {result}");
    println!("Benchmark: {PASSES} passes (sum_squares 100000 each)");

    let bench_n: i64 = 100_000;

    let t0 = bench_time_us();
    let mut bench_sum: i64 = 0;
    for _ in 0..PASSES {
        bench_sum += sum_squares(black_box(bench_n));
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Bench sum = {bench_sum}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);

    black_box(bench_sum);
}

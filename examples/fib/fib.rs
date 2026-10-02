// Rust Fibonacci benchmark - same workload as fib.c (fib(35) x 10M).

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

fn fib(n: i32) -> i64 {
    if n <= 1 {
        return n as i64;
    }
    let mut a: i64 = 0;
    let mut b: i64 = 1;
    for _ in 2..=n {
        let next = a + b;
        a = b;
        b = next;
    }
    b
}

fn main() {
    const ITER: i32 = 10_000_000;

    let t0 = bench_time_us();
    let mut bench_sum: i64 = 0;
    for _ in 0..ITER {
        bench_sum += fib(35);
    }
    let elapsed_us = bench_time_us() - t0;

    let check = fib(35);

    println!("Fibonacci 0..35:");
    let mut a: i64 = 0;
    let mut b: i64 = 1;
    print!("{a}");
    for _ in 1..=35 {
        print!(" {b}");
        let next = a + b;
        a = b;
        b = next;
    }
    println!();

    println!("Benchmark: fib(35) x 10,000,000");
    println!("fib(35) = {check}");
    println!("Bench sum mod check = {}", bench_sum % check);
    println!("Time: {elapsed_us} us");
    println!("Per call: ~{} ns", elapsed_us * 1000 / ITER as u64);

    black_box(bench_sum);
}

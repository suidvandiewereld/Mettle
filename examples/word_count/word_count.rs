// Rust word count benchmark - same workload as word_count.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r')
}

fn word_count(buf: &[u8]) -> i64 {
    let mut count: i64 = 0;
    let mut in_word = false;
    for &c in buf {
        if is_space(c) {
            in_word = false;
        } else if !in_word {
            count += 1;
            in_word = true;
        }
    }
    count
}

fn main() {
    const BUF_SIZE: usize = 262_144;
    const PASSES: i32 = 200;
    let template: &[u8] = b"a b ";

    let mut buf = vec![0u8; BUF_SIZE];
    let mut pos = 0usize;
    while pos < BUF_SIZE {
        let chunk = (BUF_SIZE - pos).min(template.len());
        buf[pos..pos + chunk].copy_from_slice(&template[..chunk]);
        pos += chunk;
    }

    let wc = word_count(&buf);

    let t0 = bench_time_us();
    let mut total: i64 = 0;
    for p in 0..PASSES {
        total += word_count(&buf);
        black_box(p);
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Word count: 256 KB buffer (a b pattern)");
    println!("Words = {wc}");
    println!("Benchmark: {PASSES} passes (word_count)");
    println!("Total words = {total}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);

    black_box(total);
}

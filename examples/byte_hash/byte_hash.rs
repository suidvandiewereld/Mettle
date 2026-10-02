// Rust byte-hash benchmark - same workload as byte_hash.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

const BUF_SIZE: i64 = 262_144;
const TEMPLATE: &[u8] = b"a b c d e f g h ";
const PASSES: i32 = 200;

fn byte_hash(buf: &[u8]) -> i64 {
    let mut hash: i64 = 5381;
    for &b in buf {
        hash = hash * 33 + i64::from(b);
    }
    hash
}

fn main() {
    let mut buf = vec![0u8; BUF_SIZE as usize];
    let mut pos: i64 = 0;
    while pos < BUF_SIZE {
        let chunk = ((BUF_SIZE - pos) as usize).min(TEMPLATE.len());
        buf[pos as usize..pos as usize + chunk].copy_from_slice(&TEMPLATE[..chunk]);
        pos += chunk as i64;
    }

    println!("Byte hash (djb2): 256 KB buffer");

    let hash = byte_hash(&buf);
    println!("Hash = {hash}");
    println!("Benchmark: {PASSES} passes (byte_hash)");

    let t0 = bench_time_us();
    let mut bench_sum: i64 = 0;
    for p in 0..PASSES {
        bench_sum += byte_hash(&buf);
        black_box(p);
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Bench sum = {bench_sum}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);
}

// Rust matrix-multiply benchmark - same workload as matrix_mul.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

const N: usize = 32;
const N2: usize = N * N;
const PASSES: i32 = 200;

fn mat_idx(row: usize, col: usize) -> usize {
    row * N + col
}

fn fill_matrix(m: &mut [i32; N2], seed: i32) {
    for row in 0..N {
        for col in 0..N {
            m[mat_idx(row, col)] = (row as i32 * 131 + col as i32 * 17 + seed).rem_euclid(251);
        }
    }
}

fn matmul(a: &[i32; N2], b: &[i32; N2], c: &mut [i32; N2]) {
    for row in 0..N {
        for col in 0..N {
            let mut sum = 0i32;
            for k in 0..N {
                sum += a[mat_idx(row, k)] * b[mat_idx(k, col)];
            }
            c[mat_idx(row, col)] = sum;
        }
    }
}

fn trace_matrix(m: &[i32; N2]) -> i64 {
    let mut sum = 0i64;
    for i in 0..N {
        sum += i64::from(m[mat_idx(i, i)]);
    }
    sum
}

fn main() {
    let mut a = [0i32; N2];
    let mut b = [0i32; N2];
    let mut c = [0i32; N2];

    fill_matrix(&mut a, 3);
    fill_matrix(&mut b, 7);

    println!("Matrix multiply: 32x32 int32 (naive)");

    matmul(&a, &b, &mut c);
    let check = trace_matrix(&c);
    println!("Trace = {check}");
    println!("Benchmark: {PASSES} passes (matmul)");

    let t0 = bench_time_us();
    let mut bench_sum = 0i64;
    for p in 0..PASSES {
        matmul(&a, &b, &mut c);
        bench_sum += trace_matrix(&c);
        black_box(p);
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Bench sum = {bench_sum}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);
}

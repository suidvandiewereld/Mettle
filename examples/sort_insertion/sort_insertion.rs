// Rust insertion-sort benchmark - same workload as sort_insertion.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

const DATA_LEN: usize = 512;
const PASSES: i32 = 200;

fn fill_data(data: &mut [i32; DATA_LEN], seed: i32) {
    for (i, value) in data.iter_mut().enumerate() {
        *value = (i as i32 * 1103515245 + seed + 12345).rem_euclid(1024);
    }
}

fn insertion_sort(data: &mut [i32]) {
    for i in 1..data.len() {
        let key = data[i];
        let mut j = i;
        while j > 0 && data[j - 1] > key {
            data[j] = data[j - 1];
            j -= 1;
        }
        data[j] = key;
    }
}

fn sum_array(data: &[i32]) -> i64 {
    data.iter().map(|v| i64::from(*v)).sum()
}

fn main() {
    let mut data = [0i32; DATA_LEN];
    let mut scratch = [0i32; DATA_LEN];

    fill_data(&mut data, 42);

    println!("Insertion sort: 512 int32 values");

    scratch.copy_from_slice(&data);
    insertion_sort(&mut scratch);
    let check = sum_array(&scratch);
    println!("Sorted sum = {check}");
    println!("Benchmark: {PASSES} passes (insertion_sort)");

    let t0 = bench_time_us();
    let mut bench_sum = 0i64;
    for p in 0..PASSES {
        scratch.copy_from_slice(&data);
        insertion_sort(&mut scratch);
        bench_sum += sum_array(&scratch);
        black_box(p);
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Bench sum = {bench_sum}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / PASSES as u64);
}

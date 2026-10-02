// Rust grep benchmark - same algorithm as grep.c.

#[path = "../bench_time.rs"]
mod bench_time;

use bench_time::bench_time_us;
use std::hint::black_box;

const PATTERN: &[u8] = b"ERROR";
const PATTERN_LEN: i64 = 5;
const PATTERN_U64: u64 = 0x524F_5252_45;
const PATTERN_MASK: u64 = 0xFFFF_FFFF_FF;
const GREP_BUF_SIZE: i64 = 1_048_576;
const GREP_PASSES: i32 = 200;

fn pattern_matches(buf: &[u8], i: i64, len: i64) -> bool {
    if i + PATTERN_LEN > len {
        return false;
    }
    let i = i as usize;
    let len = len as usize;
    if i + 8 <= len {
        let mut val = [0u8; 8];
        val.copy_from_slice(&buf[i..i + 8]);
        let loaded = u64::from_le_bytes(val);
        return (loaded & PATTERN_MASK) == PATTERN_U64;
    }
    &buf[i..i + PATTERN.len()] == PATTERN
}

#[inline(never)]
fn grep_count(buf: &[u8]) -> i64 {
    let len = buf.len() as i64;
    let mut count: i64 = 0;
    let mut found = false;
    for i in 0..len {
        let c = buf[i as usize];
        if c == b'\n' {
            if found {
                count += 1;
            }
            found = false;
        } else if !found && c == b'E' && pattern_matches(buf, i, len) {
            found = true;
        }
    }
    if found {
        count += 1;
    }
    count
}

fn main() {
    let block = b"INFO 12345\nERROR 12345\nINFO 12345\nINFO 12345\nINFO 1234\n";
    let mut buf = vec![0u8; GREP_BUF_SIZE as usize];
    let mut pos: i64 = 0;
    while pos < GREP_BUF_SIZE {
        let chunk = (GREP_BUF_SIZE - pos).min(block.len() as i64) as usize;
        buf[pos as usize..pos as usize + chunk].copy_from_slice(&block[..chunk]);
        pos += chunk as i64;
    }

    println!("Grep: count lines containing ERROR (1 MiB log buffer)");

    let matches = grep_count(&buf);
    println!("Matches = {matches}");
    println!("Benchmark: {GREP_PASSES} passes (grep_count)");

    let t0 = bench_time_us();
    let mut total: i64 = 0;
    for p in 0..GREP_PASSES {
        let off = (p & 127) as usize;
        total += grep_count(&buf[off..]);
        black_box(p);
    }
    let elapsed_us = bench_time_us() - t0;

    println!("Total matches = {total}");
    println!("Time: {elapsed_us} us");
    println!("Per pass: ~{} us", elapsed_us / GREP_PASSES as u64);

    black_box(total);
}

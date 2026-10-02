// High-resolution benchmark timing (QueryPerformanceCounter on Windows).

#[cfg(windows)]
pub fn bench_time_us() -> u64 {
    use std::sync::OnceLock;

    static FREQ: OnceLock<i64> = OnceLock::new();

    unsafe extern "system" {
        fn QueryPerformanceFrequency(frequency: *mut i64) -> i32;
        fn QueryPerformanceCounter(counter: *mut i64) -> i32;
    }

    let freq = *FREQ.get_or_init(|| {
        let mut f: i64 = 0;
        unsafe {
            QueryPerformanceFrequency(&mut f);
        }
        f
    });

    let mut counter: i64 = 0;
    unsafe {
        QueryPerformanceCounter(&mut counter);
    }

    if freq == 0 {
        return 0;
    }

    ((counter as u64) * 1_000_000) / (freq as u64)
}

#[cfg(not(windows))]
pub fn bench_time_us() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};

    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    now.as_secs() * 1_000_000 + u64::from(now.subsec_micros())
}

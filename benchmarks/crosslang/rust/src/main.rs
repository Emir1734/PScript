use std::env;
use std::time::Instant;

fn run_loops() -> i64 {
    let n = 60_000i64;
    let mut s = 0i64;
    for i in 0..n {
        s += 1;
    }
    s
}

fn run_num_lu_divmod() -> i64 {
    let n = 12_000i64;
    let mut total = 0i64;
    for i in 1..n {
        total += (i / 3) + (i % 7);
    }
    total
}

fn run_closure_call() -> i64 {
    let n = 25_000i64;
    let base = 100i64;
    let add = |x: i64| base + x;
    let mut total = 0i64;
    for i in 0..n {
        total += add(i);
    }
    total
}

fn run_closure_nested() -> i64 {
    let n = 25_000i64;
    let outer = |x: i64| {
        move |y: i64| {
            move |z: i64| x + y + z
        }
    };
    let g = outer(10)(20);
    let mut total = 0i64;
    for i in 0..n {
        total += g(i);
    }
    total
}

fn sum_recursive(n: i64) -> i64 {
    if n <= 0 {
        0
    } else {
        n + sum_recursive(n - 1)
    }
}

fn run_closure_recursive() -> i64 {
    sum_recursive(200)
}

fn run_closure_recursive_iter() -> i64 {
    let mut acc = 0i64;
    for n in (1..=200).rev() {
        acc += n;
    }
    acc
}

fn median_ms(mut body: impl FnMut() -> i64, warmup: usize, repeats: usize) -> (f64, i64) {
    for _ in 0..warmup {
        body();
    }
    let mut samples = Vec::with_capacity(repeats);
    let mut last = 0i64;
    for _ in 0..repeats {
        let start = Instant::now();
        last = body();
        let elapsed = start.elapsed().as_secs_f64() * 1000.0;
        samples.push(elapsed);
    }
    samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let median = samples[repeats / 2];
    (median, last)
}

fn main() {
    let workload = env::args().nth(1).unwrap_or_else(|| {
        eprintln!("usage: crosslang_bench <workload>");
        std::process::exit(2);
    });
    let (median, checksum) = match workload.as_str() {
        "loops" => median_ms(run_loops, 3, 7),
        "num_lu_divmod" => median_ms(run_num_lu_divmod, 3, 7),
        "closure_call" => median_ms(run_closure_call, 3, 7),
        "closure_nested" => median_ms(run_closure_nested, 3, 7),
        "closure_recursive" => median_ms(run_closure_recursive, 3, 7),
        "closure_recursive_iter" => median_ms(run_closure_recursive_iter, 3, 7),
        "verify" => {
            println!("loops\t{}", run_loops());
            println!("num_lu_divmod\t{}", run_num_lu_divmod());
            println!("closure_call\t{}", run_closure_call());
            println!("closure_nested\t{}", run_closure_nested());
            println!("closure_recursive\t{}", run_closure_recursive());
            println!("closure_recursive_iter\t{}", run_closure_recursive_iter());
            return;
        }
        _ => {
            eprintln!("unsupported workload: {}", workload);
            std::process::exit(2);
        }
    };
    println!("rust\t{}\t{:.6}\t{}", workload, median, checksum);
}

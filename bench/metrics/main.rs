use prometheus::{Counter, CounterVec, Encoder, Histogram, HistogramOpts, Opts, Registry, TextEncoder};

// 10 million requests counted by method and status, with their time in a
// histogram, then 2,000 series written out 200 times.
fn main() {
    let reg = Registry::new();
    let requests = CounterVec::new(Opts::new("requests_total", "Requests."), &["method", "code"]).unwrap();
    let seconds = Histogram::with_opts(HistogramOpts::new("request_seconds", "Request time.")).unwrap();
    let total = Counter::new("bytes_total", "Bytes.").unwrap();
    reg.register(Box::new(requests.clone())).unwrap();
    reg.register(Box::new(seconds.clone())).unwrap();
    reg.register(Box::new(total.clone())).unwrap();
    let methods = ["GET", "POST", "PUT", "DELETE"];
    let codes = ["200", "201", "404", "500", "503"];
    for i in 0..10_000_000usize {
        requests.with_label_values(&[methods[i % 4], codes[i % 5]]).inc();
        seconds.observe((i % 1000) as f64 / 1000.0);
        total.inc_by((i % 1500) as f64);
    }
    let paths = CounterVec::new(Opts::new("path_hits_total", "Hits by path."), &["path"]).unwrap();
    reg.register(Box::new(paths.clone())).unwrap();
    for i in 0..2000 {
        let p = format!("/items/{i}");
        paths.with_label_values(&[p.as_str()]).inc_by(i as f64);
    }
    let enc = TextEncoder::new();
    let mut size = 0;
    for _ in 0..200 {
        let mut buf = Vec::new();
        enc.encode(&reg.gather(), &mut buf).unwrap();
        size += buf.len();
    }
    println!("{} {} {} {}", requests.with_label_values(&["GET", "200"]).get(), seconds.get_sample_count(), total.get(), size);
}

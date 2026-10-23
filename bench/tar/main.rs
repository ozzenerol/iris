use std::io::Read;

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// The zip benchmark's 2000 files into a tar in memory, then read back.
fn main() {
    let mut seed = 1i64;
    let mut b = tar::Builder::new(Vec::new());
    for i in 0..2000 {
        let size = (next(&mut seed) % 20000 + 100) as usize;
        let mut data = String::new();
        while data.len() < size {
            data.push_str(WORDS[(next(&mut seed) % 10) as usize]);
        }
        let mut h = tar::Header::new_ustar();
        h.set_size(data.len() as u64);
        h.set_mode(0o644);
        h.set_cksum();
        b.append_data(&mut h, format!("dir{}/file{}.txt", i % 20, i), data.as_bytes()).unwrap();
    }
    let buf = b.into_inner().unwrap();
    let mut a = tar::Archive::new(&buf[..]);
    let (mut count, mut total, mut sum) = (0i64, 0i64, 0i64);
    for e in a.entries().unwrap() {
        let mut e = e.unwrap();
        let mut data = Vec::new();
        e.read_to_end(&mut data).unwrap();
        count += 1;
        total += data.len() as i64;
        sum += data.iter().map(|&x| x as i64).sum::<i64>();
    }
    println!("{} {} {} {}", count, total, sum, buf.len());
}

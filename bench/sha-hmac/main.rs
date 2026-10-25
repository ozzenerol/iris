use hmac::{Hmac, Mac};
use sha2::{Digest, Sha384};

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// SHA-384 of a 64 KiB block 4000 times, then HMAC-SHA384 of a million
// 64-byte messages, each keyed with the one before.
fn main() {
    let mut seed = 1;
    let mut chunk = String::new();
    while chunk.len() < 65536 {
        chunk.push_str(WORDS[(next(&mut seed) % 10) as usize]);
    }
    let chunk = &chunk.as_bytes()[..65536];
    let mut acc = 0i64;
    for i in 0..4000 {
        let h = Sha384::digest(chunk);
        acc += h[i % 48] as i64;
    }
    let mut key: [u8; 48] = Sha384::digest(b"key").into();
    for i in 0..1000000 {
        let at = i * 7 % 60000;
        let mut m = <Hmac<Sha384> as Mac>::new_from_slice(&key).unwrap();
        m.update(&chunk[at..at + 64]);
        key = m.finalize().into_bytes().into();
    }
    let t: String = key.iter().map(|b| format!("{:02x}", b)).collect();
    println!("{} {}", acc, t);
}

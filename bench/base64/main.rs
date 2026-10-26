use data_encoding::BASE64;

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// A 64 KiB block to base64 and back, 2000 times.
fn main() {
    let mut seed = 1;
    let mut text = String::new();
    while text.len() < 65536 {
        text.push_str(WORDS[(next(&mut seed) % 10) as usize]);
    }
    let chunk = &text.as_bytes()[..65536];
    let mut total = 0usize;
    for i in 0..2000 {
        let text = BASE64.encode(chunk);
        let back = BASE64.decode(text.as_bytes()).unwrap();
        total += text.len() + back[i % back.len()] as usize;
    }
    println!("{}", total);
}

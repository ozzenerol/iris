use std::io::{Cursor, Read, Write};
use zip::write::SimpleFileOptions;

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

const WORDS: [&str; 10] = ["alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"];

// 2000 files of mixed sizes, deflated into a zip in memory, then read back
// with each CRC checked.
fn main() {
    let mut seed = 1i64;
    let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opts = SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    for i in 0..2000 {
        let size = (next(&mut seed) % 20000 + 100) as usize;
        let mut data = String::new();
        while data.len() < size {
            data.push_str(WORDS[(next(&mut seed) % 10) as usize]);
        }
        w.start_file(format!("dir{}/file{}.txt", i % 20, i), opts).unwrap();
        w.write_all(data.as_bytes()).unwrap();
    }
    let buf = w.finish().unwrap().into_inner();
    let mut a = zip::ZipArchive::new(Cursor::new(&buf)).unwrap();
    let (mut total, mut crcs) = (0i64, 0i64);
    let n = a.len();
    for k in 0..n {
        let mut f = a.by_index(k).unwrap();
        let mut data = Vec::new();
        f.read_to_end(&mut data).unwrap(); // checks the CRC at the end
        total += data.len() as i64;
        crcs += crc32fast::hash(&data) as i64;
    }
    println!("{} {} {}", n, total, crcs);
}

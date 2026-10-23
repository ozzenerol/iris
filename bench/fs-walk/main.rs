use std::fs;
use std::path::Path;

fn walk(dir: &Path, count: &mut u64, bytes: &mut u64) {
    for e in fs::read_dir(dir).unwrap() {
        let e = e.unwrap();
        let t = e.file_type().unwrap();
        if t.is_dir() {
            walk(&e.path(), count, bytes);
        } else {
            *count += 1;
            *bytes += e.metadata().unwrap().len();
        }
    }
}

// 50 folders of 10 folders of 100 files: 50,000 files
fn main() {
    let root = std::env::temp_dir().join(format!("rust-bench-walk-{}", std::process::id()));
    let data = [b'x'; 100];
    let mut n = 0;
    for a in 0..50 {
        for b in 0..10 {
            let dir = root.join(format!("d{}/e{}", a, b));
            fs::create_dir_all(&dir).unwrap();
            for c in 0..100 {
                fs::write(dir.join(format!("f{}.txt", c)), &data[..n % 100 + 1]).unwrap();
                n += 1;
            }
        }
    }
    let (mut count, mut bytes) = (0u64, 0u64);
    walk(&root, &mut count, &mut bytes);
    println!("{} {}", count, bytes);
    fs::remove_dir_all(&root).unwrap();
}

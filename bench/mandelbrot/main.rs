const N: usize = 4000;
const ITER: usize = 50;

fn main() {
    let mut inside = 0;
    for y in 0..N {
        let ci = 2.0 * y as f64 / N as f64 - 1.0;
        for x in 0..N {
            let cr = 2.0 * x as f64 / N as f64 - 1.5;
            let (mut zr, mut zi) = (0.0f64, 0.0f64);
            let mut escaped = false;
            for _ in 0..ITER {
                let tr = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = tr;
                if zr * zr + zi * zi > 4.0 {
                    escaped = true;
                    break;
                }
            }
            if !escaped {
                inside += 1;
            }
        }
    }
    println!("{}", inside);
}

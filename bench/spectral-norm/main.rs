const N: usize = 5500;

fn a(i: usize, j: usize) -> f64 {
    1.0 / ((i + j) * (i + j + 1) / 2 + i + 1) as f64
}

fn mul_av(v: &[f64], out: &mut [f64]) {
    for i in 0..v.len() {
        let mut sum = 0.0;
        for j in 0..v.len() {
            sum += a(i, j) * v[j];
        }
        out[i] = sum;
    }
}

fn mul_atv(v: &[f64], out: &mut [f64]) {
    for i in 0..v.len() {
        let mut sum = 0.0;
        for j in 0..v.len() {
            sum += a(j, i) * v[j];
        }
        out[i] = sum;
    }
}

fn mul_atav(v: &[f64], out: &mut [f64], tmp: &mut [f64]) {
    mul_av(v, tmp);
    mul_atv(tmp, out);
}

fn main() {
    let mut u = vec![1.0; N];
    let mut v = vec![0.0; N];
    let mut tmp = vec![0.0; N];
    for _ in 0..10 {
        mul_atav(&u, &mut v, &mut tmp);
        mul_atav(&v, &mut u, &mut tmp);
    }
    let (mut vbv, mut vv) = (0.0, 0.0);
    for i in 0..N {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    println!("{}", (vbv / vv).sqrt());
}

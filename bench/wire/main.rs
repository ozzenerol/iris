// The same protocol, read and written by hand with from_be_bytes and
// to_be_bytes, checking each length as the Iris and Go versions do.

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Hello = 1,
    Data = 2,
    Bye = 3,
}

struct Msg {
    version: u8,
    kind: Kind,
    flags: u16,
    seq: u32,
    user: Vec<u8>,
    ports: Vec<u16>,
}

#[derive(Debug)]
enum Error {
    Short,
    BadValue,
    Extra,
}

fn take<'a>(b: &mut &'a [u8], n: usize) -> Result<&'a [u8], Error> {
    if b.len() < n {
        return Err(Error::Short);
    }
    let (head, rest) = b.split_at(n);
    *b = rest;
    Ok(head)
}

fn decode(mut b: &[u8]) -> Result<Msg, Error> {
    let h = take(&mut b, 8)?;
    let kind = match h[1] {
        1 => Kind::Hello,
        2 => Kind::Data,
        3 => Kind::Bye,
        _ => return Err(Error::BadValue),
    };
    let flags = u16::from_be_bytes([h[2], h[3]]);
    let seq = u32::from_be_bytes([h[4], h[5], h[6], h[7]]);
    let n = take(&mut b, 1)?[0] as usize;
    let user = take(&mut b, n)?.to_vec();
    let count = take(&mut b, 1)?[0] as usize;
    let raw = take(&mut b, count * 2)?;
    let ports = raw.chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
    if !b.is_empty() {
        return Err(Error::Extra);
    }
    Ok(Msg { version: h[0], kind, flags, seq, user, ports })
}

fn encode(m: &Msg) -> Result<Vec<u8>, Error> {
    if m.user.len() > 255 || m.ports.len() > 255 {
        return Err(Error::Extra);
    }
    let mut out = Vec::with_capacity(10 + m.user.len() + 2 * m.ports.len());
    out.push(m.version);
    out.push(m.kind as u8);
    out.extend_from_slice(&m.flags.to_be_bytes());
    out.extend_from_slice(&m.seq.to_be_bytes());
    out.push(m.user.len() as u8);
    out.extend_from_slice(&m.user);
    out.push(m.ports.len() as u8);
    for p in &m.ports {
        out.extend_from_slice(&p.to_be_bytes());
    }
    Ok(out)
}

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

// 100,000 messages decoded and encoded again, 30 times.
fn main() {
    let mut seed = 1;
    let mut stream = Vec::new();
    let mut ends = Vec::new();
    for i in 0..100000u32 {
        let mut user = Vec::new();
        for j in 0..(3 + next(&mut seed) % 10) as usize {
            user.push(b"abcdefghij"[j % 10]);
        }
        let n = (next(&mut seed) % 5) as usize;
        let mut ports = Vec::with_capacity(n);
        for _ in 0..n {
            ports.push((next(&mut seed) % 65536) as u16);
        }
        let m = Msg { version: 1, kind: Kind::Data, flags: (i % 7) as u16, seq: i, user, ports };
        stream.extend_from_slice(&encode(&m).unwrap());
        ends.push(stream.len());
    }
    let mut total: i64 = 0;
    for _ in 0..30 {
        let mut start = 0;
        for &end in &ends {
            let m = decode(&stream[start..end]).unwrap();
            total += m.seq as i64 + m.user.len() as i64 + m.flags as i64;
            for p in &m.ports {
                total += *p as i64;
            }
            let b = encode(&m).unwrap();
            total += b.len() as i64;
            start = end;
        }
    }
    println!("{}", total);
}

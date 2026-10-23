struct Node {
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

fn make(depth: u32) -> Box<Node> {
    if depth > 0 {
        Box::new(Node { left: Some(make(depth - 1)), right: Some(make(depth - 1)) })
    } else {
        Box::new(Node { left: None, right: None })
    }
}

fn check(n: &Node) -> i64 {
    match (&n.left, &n.right) {
        (Some(l), Some(r)) => 1 + check(l) + check(r),
        _ => 1,
    }
}

const MAX_DEPTH: u32 = 21;

fn main() {
    println!("stretch tree of depth {}\t check: {}", MAX_DEPTH + 1, check(&make(MAX_DEPTH + 1)));
    let long = make(MAX_DEPTH);
    let mut d = 4;
    while d <= MAX_DEPTH {
        let iterations = 1 << (MAX_DEPTH - d + 4);
        let mut sum = 0;
        for _ in 0..iterations {
            sum += check(&make(d));
        }
        println!("{}\t trees of depth {}\t check: {}", iterations, d, sum);
        d += 2;
    }
    println!("long lived tree of depth {}\t check: {}", MAX_DEPTH, check(&long));
}

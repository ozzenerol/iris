const ROUNDS: usize = 2_000_000;
const WORKERS: usize = 1000;
const JOBS: i64 = 5_000_000;

#[tokio::main]
async fn main() {
    let (ping_tx, mut ping_rx) = tokio::sync::mpsc::channel::<i64>(1);
    let (back_tx, mut back_rx) = tokio::sync::mpsc::channel::<i64>(1);
    tokio::spawn(async move {
        while let Some(v) = ping_rx.recv().await {
            back_tx.send(v + 1).await.unwrap();
            if v < 0 {
                return;
            }
        }
    });
    let mut v = 0i64;
    for _ in 0..ROUNDS {
        ping_tx.send(v).await.unwrap();
        v = back_rx.recv().await.unwrap();
    }
    ping_tx.send(-1).await.unwrap();
    back_rx.recv().await.unwrap();

    let (jobs_tx, jobs_rx) = async_channel::bounded::<i64>(1024);
    let (res_tx, res_rx) = async_channel::bounded::<i64>(WORKERS);
    for _ in 0..WORKERS {
        let jobs = jobs_rx.clone();
        let results = res_tx.clone();
        tokio::spawn(async move {
            let mut sum = 0i64;
            loop {
                let v = jobs.recv().await.unwrap();
                if v < 0 {
                    results.send(sum).await.unwrap();
                    return;
                }
                sum += v;
            }
        });
    }
    for i in 0..JOBS {
        jobs_tx.send(i).await.unwrap();
    }
    for _ in 0..WORKERS {
        jobs_tx.send(-1).await.unwrap();
    }
    let mut total = 0i64;
    for _ in 0..WORKERS {
        total += res_rx.recv().await.unwrap();
    }
    println!("{} {}", v, total);
}

use procfs::process::Process;
use procfs::{Current, CurrentSI, KernelStats, LoadAverage, Meminfo};

// The machine's numbers read 20,000 times: the CPU time of each core,
// memory, load, network interfaces, disks, mounts and this process.
fn main() {
    let mut total = 0usize;
    for _ in 0..20000 {
        total += KernelStats::current().unwrap().cpu_time.len();
        total += (Meminfo::current().unwrap().mem_total > 0) as usize;
        total += (LoadAverage::current().unwrap().one >= 0.0) as usize;
        total += procfs::net::dev_status().unwrap().len();
        total += procfs::diskstats().unwrap().len();
        total += procfs::mounts().unwrap().len();
        total += (Process::myself().unwrap().stat().unwrap().rss > 0) as usize;
    }
    println!("{}", total);
}

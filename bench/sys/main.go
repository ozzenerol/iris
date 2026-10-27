package main

import (
	"fmt"

	"github.com/prometheus/procfs"
	"github.com/prometheus/procfs/blockdevice"
)

// The machine's numbers read 20,000 times: the CPU time of each core,
// memory, load, network interfaces, disks, mounts and this process.
func main() {
	fs, err := procfs.NewDefaultFS()
	if err != nil {
		panic(err)
	}
	bfs, err := blockdevice.NewDefaultFS()
	if err != nil {
		panic(err)
	}
	total := 0
	for i := 0; i < 20000; i++ {
		st, err := fs.Stat()
		if err != nil {
			panic(err)
		}
		total += len(st.CPU)
		mi, err := fs.Meminfo()
		if err != nil {
			panic(err)
		}
		if *mi.MemTotal > 0 {
			total++
		}
		la, err := fs.LoadAvg()
		if err != nil {
			panic(err)
		}
		if la.Load1 >= 0 {
			total++
		}
		nd, err := fs.NetDev()
		if err != nil {
			panic(err)
		}
		total += len(nd)
		ds, err := bfs.ProcDiskstats()
		if err != nil {
			panic(err)
		}
		total += len(ds)
		ms, err := procfs.GetMounts()
		if err != nil {
			panic(err)
		}
		total += len(ms)
		p, err := fs.Self()
		if err != nil {
			panic(err)
		}
		s, err := p.Stat()
		if err != nil {
			panic(err)
		}
		if s.RSS > 0 {
			total++
		}
	}
	fmt.Println(total)
}

// Load generator for tcp-echo and the proxies: conns connections, each
// sends a message (64 bytes unless -s says otherwise) and waits for it to
// come back, for the given time. Prints msg/s.
package main

import (
	"flag"
	"fmt"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:9000", "server")
	conns := flag.Int("c", 100, "connections")
	dur := flag.Duration("d", 10*time.Second, "duration")
	size := flag.Int("s", 64, "message size")
	flag.Parse()
	var total atomic.Int64
	var stop atomic.Bool
	var wg sync.WaitGroup
	msg := make([]byte, *size)
	for i := range msg {
		msg[i] = 'a' + byte(i%26)
	}
	for i := 0; i < *conns; i++ {
		c, err := net.Dial("tcp", *addr)
		if err != nil {
			panic(err)
		}
		wg.Add(1)
		go func(c net.Conn) {
			defer wg.Done()
			defer c.Close()
			buf := make([]byte, *size)
			n := int64(0)
			for !stop.Load() {
				if _, err := c.Write(msg); err != nil {
					break
				}
				if _, err := io.ReadFull(c, buf); err != nil {
					break
				}
				n++
			}
			total.Add(n)
		}(c)
	}
	time.Sleep(*dur)
	stop.Store(true)
	wg.Wait()
	fmt.Printf("%.0f\n", float64(total.Load())/dur.Seconds())
}

package main

import "net"

func echo(c net.Conn) {
	defer c.Close()
	buf := make([]byte, 4096)
	for {
		n, err := c.Read(buf)
		if err != nil || n == 0 {
			return
		}
		if _, err := c.Write(buf[:n]); err != nil {
			return
		}
	}
}

func main() {
	ln, err := net.Listen("tcp", "127.0.0.1:9000")
	if err != nil {
		panic(err)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		go echo(c)
	}
}

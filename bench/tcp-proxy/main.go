package main

import (
	"io"
	"net"
)

func handle(c *net.TCPConn) {
	defer c.Close()
	d, err := net.Dial("tcp", "127.0.0.1:9001")
	if err != nil {
		return
	}
	b := d.(*net.TCPConn)
	defer b.Close()
	done := make(chan struct{}, 1)
	go func() {
		io.Copy(b, c)
		b.CloseWrite()
		done <- struct{}{}
	}()
	io.Copy(c, b)
	c.CloseWrite()
	<-done
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
		go handle(c.(*net.TCPConn))
	}
}

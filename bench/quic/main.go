// A QUIC server and client in one program, over loopback: 200 connections,
// each with a handshake and a request on a stream, then 512 MiB through one
// stream, with quic-go. Go picks TLS 1.3's cipher suite itself: AES-128-GCM
// on a CPU with AES instructions.
package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"

	"github.com/quic-go/quic-go"
)

func serveStream(s *quic.Stream) {
	buf := make([]byte, 64<<10)
	total := 0
	for {
		n, err := s.Read(buf)
		total += n
		if err != nil {
			break
		}
	}
	if total == 5 {
		s.Write([]byte("ok"))
	} else {
		s.Write([]byte(strconv.Itoa(total)))
	}
	s.Close()
}

func serveConn(c *quic.Conn) {
	for {
		s, err := c.AcceptStream(context.Background())
		if err != nil {
			return
		}
		go serveStream(s)
	}
}

func main() {
	exe, _ := os.Executable()
	data := filepath.Join(filepath.Dir(exe), "..", "..", "tests", "data", "tls")
	cert, err := tls.LoadX509KeyPair(filepath.Join(data, "server.pem"), filepath.Join(data, "server.key"))
	if err != nil {
		panic(err)
	}
	caPem, _ := os.ReadFile(filepath.Join(data, "ca.pem"))
	pool := x509.NewCertPool()
	pool.AppendCertsFromPEM(caPem)
	conf := &quic.Config{MaxIncomingStreams: 100}
	ln, err := quic.ListenAddr("127.0.0.1:0", &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"bench"}}, conf)
	if err != nil {
		panic(err)
	}
	go func() {
		for {
			c, err := ln.Accept(context.Background())
			if err != nil {
				return
			}
			go serveConn(c)
		}
	}()
	addr := ln.Addr().String()
	ctls := &tls.Config{RootCAs: pool, ServerName: "localhost", NextProtos: []string{"bench"}}
	ctx := context.Background()

	good := 0
	for i := 0; i < 200; i++ {
		c, err := quic.DialAddr(ctx, addr, ctls, conf)
		if err != nil {
			panic(err)
		}
		s, _ := c.OpenStreamSync(ctx)
		s.Write([]byte("hello"))
		s.Close()
		reply, _ := io.ReadAll(s)
		if string(reply) == "ok" {
			good++
		}
		c.CloseWithError(0, "")
	}

	c, err := quic.DialAddr(ctx, addr, ctls, conf)
	if err != nil {
		panic(err)
	}
	s, _ := c.OpenStreamSync(ctx)
	block := make([]byte, 1<<20)
	for i := range block {
		block[i] = byte(i * 7)
	}
	for i := 0; i < 512; i++ {
		if _, err := s.Write(block); err != nil {
			panic(err)
		}
	}
	s.Close()
	counted, _ := io.ReadAll(s)
	c.CloseWithError(0, "")
	fmt.Printf("connections %d, bytes %s\n", good, counted)
}

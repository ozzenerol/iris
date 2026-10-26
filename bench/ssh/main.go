package main

// An SSH server and client in one program, over loopback: 100 logins with
// an Ed25519 key, each running a command, then 512 MiB through one
// channel. mlkem768x25519-sha256 and AES-256-GCM, as the Iris and Rust
// versions use.

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"fmt"
	"io"
	"net"
	"strconv"

	"golang.org/x/crypto/ssh"
)

var algorithms = ssh.Config{
	KeyExchanges: []string{"mlkem768x25519-sha256"},
	Ciphers:      []string{"aes256-gcm@openssh.com"},
}

func handle(ch ssh.Channel, reqs <-chan *ssh.Request) {
	defer ch.Close()
	for req := range reqs {
		if req.Type != "exec" {
			req.Reply(false, nil)
			continue
		}
		req.Reply(true, nil)
		cmd := string(req.Payload[4:])
		if cmd == "count" {
			n, _ := io.Copy(io.Discard, ch)
			ch.Write([]byte(strconv.FormatInt(n, 10)))
		} else {
			ch.Write([]byte("ok"))
		}
		ch.SendRequest("exit-status", false, []byte{0, 0, 0, 0})
		return
	}
}

func serve(ln net.Listener, config *ssh.ServerConfig) {
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		go func() {
			_, chans, reqs, err := ssh.NewServerConn(c, config)
			if err != nil {
				return
			}
			go ssh.DiscardRequests(reqs)
			for nc := range chans {
				ch, r, err := nc.Accept()
				if err != nil {
					continue
				}
				go handle(ch, r)
			}
		}()
	}
}

func main() {
	_, hostPriv, _ := ed25519.GenerateKey(rand.Reader)
	_, userPriv, _ := ed25519.GenerateKey(rand.Reader)
	hostSigner, _ := ssh.NewSignerFromKey(hostPriv)
	userSigner, _ := ssh.NewSignerFromKey(userPriv)
	userPub := userSigner.PublicKey().Marshal()

	server := &ssh.ServerConfig{
		Config: algorithms,
		PublicKeyCallback: func(m ssh.ConnMetadata, k ssh.PublicKey) (*ssh.Permissions, error) {
			if bytes.Equal(k.Marshal(), userPub) {
				return nil, nil
			}
			return nil, fmt.Errorf("unknown key")
		},
	}
	server.AddHostKey(hostSigner)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	go serve(ln, server)
	client := &ssh.ClientConfig{
		Config:          algorithms,
		User:            "bench",
		Auth:            []ssh.AuthMethod{ssh.PublicKeys(userSigner)},
		HostKeyCallback: ssh.FixedHostKey(hostSigner.PublicKey()),
	}

	logins := 0
	for i := 0; i < 100; i++ {
		c, err := ssh.Dial("tcp", ln.Addr().String(), client)
		if err != nil {
			panic(err)
		}
		s, _ := c.NewSession()
		out, _ := s.Output("hello")
		if string(out) == "ok" {
			logins++
		}
		c.Close()
	}

	c, err := ssh.Dial("tcp", ln.Addr().String(), client)
	if err != nil {
		panic(err)
	}
	defer c.Close()
	s, _ := c.NewSession()
	in, _ := s.StdinPipe()
	outPipe, _ := s.StdoutPipe()
	if err := s.Start("count"); err != nil {
		panic(err)
	}
	block := make([]byte, 1<<20)
	for i := range block {
		block[i] = byte(i * 7 & 255)
	}
	for i := 0; i < 512; i++ {
		in.Write(block)
	}
	in.Close()
	counted, _ := io.ReadAll(outPipe)
	s.Wait()
	fmt.Printf("logins %d, bytes %s\n", logins, counted)
}

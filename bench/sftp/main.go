package main

// An SFTP server and client in one program, over loopback, through one
// SSH connection: 1000 small files written, stat'ed, read back and
// removed, then a 256 MiB file uploaded and downloaded. pkg/sftp's server
// keeps the files in a temporary folder; its client writes and reads with
// 64 requests at once, as the Iris one does. mlkem768x25519-sha256 and
// AES-256-GCM, as the Iris and Rust versions use.

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"fmt"
	"io"
	"net"
	"os"

	"github.com/pkg/sftp"
	"golang.org/x/crypto/ssh"
)

var algorithms = ssh.Config{
	KeyExchanges: []string{"mlkem768x25519-sha256"},
	Ciphers:      []string{"aes256-gcm@openssh.com"},
}

func handle(ch ssh.Channel, reqs <-chan *ssh.Request, root string) {
	defer ch.Close()
	for req := range reqs {
		if req.Type != "subsystem" || string(req.Payload[4:]) != "sftp" {
			req.Reply(false, nil)
			continue
		}
		req.Reply(true, nil)
		go ssh.DiscardRequests(reqs)
		s, err := sftp.NewServer(ch, sftp.WithServerWorkingDirectory(root))
		if err != nil {
			return
		}
		s.Serve()
		s.Close()
		return
	}
}

func serve(ln net.Listener, config *ssh.ServerConfig, root string) {
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
				go handle(ch, r, root)
			}
		}()
	}
}

func main() {
	root, err := os.MkdirTemp("", "sftp-bench")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(root)
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
	go serve(ln, server, root)
	client := &ssh.ClientConfig{
		Config:          algorithms,
		User:            "bench",
		Auth:            []ssh.AuthMethod{ssh.PublicKeys(userSigner)},
		HostKeyCallback: ssh.FixedHostKey(hostSigner.PublicKey()),
	}
	c, err := ssh.Dial("tcp", ln.Addr().String(), client)
	if err != nil {
		panic(err)
	}
	defer c.Close()
	s, err := sftp.NewClient(c, sftp.MaxConcurrentRequestsPerFile(64), sftp.UseConcurrentWrites(true))
	if err != nil {
		panic(err)
	}
	defer s.Close()

	small := make([]byte, 1024)
	for i := range small {
		small[i] = byte(i * 7 & 255)
	}
	files := 0
	for i := 0; i < 1000; i++ {
		name := fmt.Sprintf("f%d", i)
		f, err := s.Create(name)
		if err != nil {
			panic(err)
		}
		f.Write(small)
		f.Close()
		info, err := s.Stat(name)
		if err != nil {
			panic(err)
		}
		r, _ := s.Open(name)
		back, _ := io.ReadAll(r)
		r.Close()
		if info.Size() == int64(len(small)) && bytes.Equal(back, small) {
			files++
		}
		s.Remove(name)
	}

	block := make([]byte, 1<<20)
	for i := range block {
		block[i] = byte(i * 7 & 255)
	}
	big := bytes.Repeat(block, 256)
	f, err := s.Create("big")
	if err != nil {
		panic(err)
	}
	if _, err := f.ReadFrom(bytes.NewReader(big)); err != nil {
		panic(err)
	}
	f.Close()
	r, _ := s.Open("big")
	var got bytes.Buffer
	got.Grow(len(big))
	if _, err := r.WriteTo(&got); err != nil {
		panic(err)
	}
	r.Close()
	fmt.Printf("files %d, bytes %d, same %v\n", files, got.Len(), bytes.Equal(got.Bytes(), big))
}

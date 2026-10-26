package main

import (
	"crypto/aes"
	"crypto/cipher"
	"fmt"
	"hash/crc32"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// A 64 KiB block sealed with AES-256-GCM and opened again 2000 times,
// each round with its own key and nonce.
func main() {
	seed := int64(1)
	var b strings.Builder
	for b.Len() < 65536 {
		b.WriteString(words[next(&seed)%10])
	}
	chunk := []byte(b.String())[:65536]
	c := uint32(0)
	for i := 0; i < 2000; i++ {
		at := i % 997
		block, err := aes.NewCipher(chunk[at : at+32])
		if err != nil {
			panic(err)
		}
		gcm, err := cipher.NewGCM(block)
		if err != nil {
			panic(err)
		}
		nonce := chunk[at+32 : at+44]
		sealed := gcm.Seal(nil, nonce, chunk, nil)
		c = crc32.Update(c, crc32.IEEETable, sealed[len(sealed)-16:])
		back, err := gcm.Open(nil, nonce, sealed, nil)
		if err != nil {
			panic(err)
		}
		c = crc32.Update(c, crc32.IEEETable, back[i:i+1])
	}
	fmt.Println(c)
}

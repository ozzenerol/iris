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

// A 64 KiB block encrypted with AES-256 in counter mode 4000 times over,
// each round on the result of the one before with its own key.
func main() {
	seed := int64(1)
	var b strings.Builder
	for b.Len() < 65536 {
		b.WriteString(words[next(&seed)%10])
	}
	chunk := []byte(b.String())[:65536]
	data := append([]byte(nil), chunk...)
	for i := 0; i < 4000; i++ {
		at := i % 997
		block, err := aes.NewCipher(chunk[at : at+32])
		if err != nil {
			panic(err)
		}
		cipher.NewCTR(block, chunk[at+32:at+48]).XORKeyStream(data, data)
	}
	fmt.Println(crc32.ChecksumIEEE(data))
}

package main

import (
	"fmt"
	"hash/crc32"
	"strings"

	"golang.org/x/crypto/chacha20"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// A 64 KiB block encrypted with ChaCha20 4000 times over, each round on
// the result of the one before with its own key.
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
		c, err := chacha20.NewUnauthenticatedCipher(chunk[at:at+32], chunk[at+32:at+44])
		if err != nil {
			panic(err)
		}
		c.XORKeyStream(data, data)
	}
	fmt.Println(crc32.ChecksumIEEE(data))
}

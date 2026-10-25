package main

import (
	"crypto/hmac"
	"crypto/sha512"
	"encoding/hex"
	"fmt"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// SHA-384 of a 64 KiB block 4000 times, then HMAC-SHA384 of a million
// 64-byte messages, each keyed with the one before.
func main() {
	seed := int64(1)
	var b strings.Builder
	for b.Len() < 65536 {
		b.WriteString(words[next(&seed)%10])
	}
	chunk := []byte(b.String())[:65536]
	acc := int64(0)
	for i := 0; i < 4000; i++ {
		h := sha512.Sum384(chunk)
		acc += int64(h[i%48])
	}
	k := sha512.Sum384([]byte("key"))
	key := k[:]
	for i := 0; i < 1000000; i++ {
		at := i * 7 % 60000
		m := hmac.New(sha512.New384, key)
		m.Write(chunk[at : at+64])
		key = m.Sum(nil)
	}
	fmt.Println(acc, hex.EncodeToString(key))
}

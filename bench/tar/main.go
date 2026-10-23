package main

import (
	"archive/tar"
	"bytes"
	"fmt"
	"io"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// The zip benchmark's 2000 files into a tar in memory, then read back.
func main() {
	seed := int64(1)
	var buf bytes.Buffer
	w := tar.NewWriter(&buf)
	for i := 0; i < 2000; i++ {
		size := int(next(&seed)%20000 + 100)
		var data strings.Builder
		for data.Len() < size {
			data.WriteString(words[next(&seed)%10])
		}
		hdr := &tar.Header{Name: fmt.Sprintf("dir%d/file%d.txt", i%20, i), Mode: 0o644, Size: int64(data.Len()), Format: tar.FormatUSTAR}
		if err := w.WriteHeader(hdr); err != nil {
			panic(err)
		}
		io.WriteString(w, data.String())
	}
	if err := w.Close(); err != nil {
		panic(err)
	}
	size := buf.Len()
	r := tar.NewReader(bytes.NewReader(buf.Bytes()))
	var count, total, sum int64
	for {
		_, err := r.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		data, err := io.ReadAll(r)
		if err != nil {
			panic(err)
		}
		count++
		total += int64(len(data))
		for _, b := range data {
			sum += int64(b)
		}
	}
	fmt.Println(count, total, sum, size)
}

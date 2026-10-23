package main

import (
	"archive/zip"
	"bytes"
	"fmt"
	"hash/crc32"
	"io"
	"strings"
)

func next(s *int64) int64 {
	*s = (*s*1103515245 + 12345) % 2147483648
	return *s
}

var words = []string{"alpha ", "beta ", "gamma ", "delta ", "error ", "ok ", "GET /index.html ", "200 ", "404 ", "\n"}

// 2000 files of mixed sizes, deflated into a zip in memory, then read back
// with each CRC checked.
func main() {
	seed := int64(1)
	var buf bytes.Buffer
	w := zip.NewWriter(&buf)
	for i := 0; i < 2000; i++ {
		size := int(next(&seed)%20000 + 100)
		var data strings.Builder
		for data.Len() < size {
			data.WriteString(words[next(&seed)%10])
		}
		f, err := w.Create(fmt.Sprintf("dir%d/file%d.txt", i%20, i))
		if err != nil {
			panic(err)
		}
		io.WriteString(f, data.String())
	}
	if err := w.Close(); err != nil {
		panic(err)
	}
	r, err := zip.NewReader(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
	if err != nil {
		panic(err)
	}
	var total, crcs int64
	for _, f := range r.File {
		rc, err := f.Open()
		if err != nil {
			panic(err)
		}
		data, err := io.ReadAll(rc) // checks the CRC at the end
		if err != nil {
			panic(err)
		}
		rc.Close()
		total += int64(len(data))
		crcs += int64(crc32.ChecksumIEEE(data))
	}
	fmt.Println(len(r.File), total, crcs)
}

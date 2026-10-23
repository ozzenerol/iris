package main

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
)

// 50 folders of 10 folders of 100 files: 50,000 files
func main() {
	root, err := os.MkdirTemp("", "go-bench-walk")
	if err != nil {
		panic(err)
	}
	data := make([]byte, 100)
	for i := range data {
		data[i] = 'x'
	}
	n := 0
	for a := 0; a < 50; a++ {
		for b := 0; b < 10; b++ {
			dir := filepath.Join(root, fmt.Sprintf("d%d/e%d", a, b))
			if err := os.MkdirAll(dir, 0o777); err != nil {
				panic(err)
			}
			for c := 0; c < 100; c++ {
				if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("f%d.txt", c)), data[:n%100+1], 0o666); err != nil {
					panic(err)
				}
				n++
			}
		}
	}
	var count, bytes int64
	err = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() {
			info, err := d.Info()
			if err != nil {
				return err
			}
			count++
			bytes += info.Size()
		}
		return nil
	})
	if err != nil {
		panic(err)
	}
	fmt.Println(count, bytes)
	os.RemoveAll(root)
}

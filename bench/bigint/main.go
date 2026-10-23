package main

import (
	"fmt"
	"math/big"
)

func main() {
	f := big.NewInt(1)
	for i := int64(2); i <= 5000; i++ {
		f.Mul(f, big.NewInt(i))
	}
	digits := f.String()
	p := new(big.Int).Lsh(big.NewInt(1), 2048)
	m := new(big.Int).Sub(p, big.NewInt(159))
	e := big.NewInt(65537)
	check := 0
	for i := int64(0); i < 100; i++ {
		b := new(big.Int).Exp(big.NewInt(i+3), big.NewInt(300), nil)
		r := new(big.Int).Exp(b, e, m)
		check += r.BitLen()
	}
	fmt.Println(len(digits), digits[:12], check)
}

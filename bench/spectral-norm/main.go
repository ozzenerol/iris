package main

import (
	"fmt"
	"math"
)

const N = 5500

func a(i, j int) float64 { return 1.0 / float64((i+j)*(i+j+1)/2+i+1) }

func mulAv(v, out []float64) {
	for i := range v {
		sum := 0.0
		for j := range v {
			sum += a(i, j) * v[j]
		}
		out[i] = sum
	}
}

func mulAtv(v, out []float64) {
	for i := range v {
		sum := 0.0
		for j := range v {
			sum += a(j, i) * v[j]
		}
		out[i] = sum
	}
}

func mulAtAv(v, out, tmp []float64) {
	mulAv(v, tmp)
	mulAtv(tmp, out)
}

func main() {
	u := make([]float64, N)
	v := make([]float64, N)
	tmp := make([]float64, N)
	for i := range u {
		u[i] = 1
	}
	for i := 0; i < 10; i++ {
		mulAtAv(u, v, tmp)
		mulAtAv(v, u, tmp)
	}
	var vBv, vv float64
	for i := range u {
		vBv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	fmt.Println(math.Sqrt(vBv / vv))
}

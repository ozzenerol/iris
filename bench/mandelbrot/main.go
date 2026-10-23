package main

import "fmt"

const N = 4000
const iter = 50

func main() {
	inside := 0
	for y := 0; y < N; y++ {
		ci := 2.0*float64(y)/N - 1.0
		for x := 0; x < N; x++ {
			cr := 2.0*float64(x)/N - 1.5
			zr, zi := 0.0, 0.0
			escaped := false
			for k := 0; k < iter; k++ {
				tr := zr*zr - zi*zi + cr
				zi = 2.0*zr*zi + ci
				zr = tr
				if zr*zr+zi*zi > 4.0 {
					escaped = true
					break
				}
			}
			if !escaped {
				inside++
			}
		}
	}
	fmt.Println(inside)
}

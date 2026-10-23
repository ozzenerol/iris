package main

import (
	"fmt"
	"math"
)

const solarMass = 4 * math.Pi * math.Pi
const daysPerYear = 365.24

type Body struct{ x, y, z, vx, vy, vz, mass float64 }

func advance(bodies []Body, dt float64) {
	for i := 0; i < len(bodies); i++ {
		for j := i + 1; j < len(bodies); j++ {
			dx := bodies[i].x - bodies[j].x
			dy := bodies[i].y - bodies[j].y
			dz := bodies[i].z - bodies[j].z
			d2 := dx*dx + dy*dy + dz*dz
			mag := dt / (d2 * math.Sqrt(d2))
			mj := bodies[j].mass * mag
			bodies[i].vx -= dx * mj
			bodies[i].vy -= dy * mj
			bodies[i].vz -= dz * mj
			mi := bodies[i].mass * mag
			bodies[j].vx += dx * mi
			bodies[j].vy += dy * mi
			bodies[j].vz += dz * mi
		}
	}
	for i := range bodies {
		bodies[i].x += dt * bodies[i].vx
		bodies[i].y += dt * bodies[i].vy
		bodies[i].z += dt * bodies[i].vz
	}
}

func energy(bodies []Body) float64 {
	e := 0.0
	for i, b := range bodies {
		e += 0.5 * b.mass * (b.vx*b.vx + b.vy*b.vy + b.vz*b.vz)
		for j := i + 1; j < len(bodies); j++ {
			dx := b.x - bodies[j].x
			dy := b.y - bodies[j].y
			dz := b.z - bodies[j].z
			e -= b.mass * bodies[j].mass / math.Sqrt(dx*dx+dy*dy+dz*dz)
		}
	}
	return e
}

func main() {
	bodies := []Body{
		{mass: solarMass},
		{4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
			1.66007664274403694e-03 * daysPerYear, 7.69901118419740425e-03 * daysPerYear,
			-6.90460016972063023e-05 * daysPerYear, 9.54791938424326609e-04 * solarMass},
		{8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
			-2.76742510726862411e-03 * daysPerYear, 4.99852801234917238e-03 * daysPerYear,
			2.30417297573763929e-05 * daysPerYear, 2.85885980666130812e-04 * solarMass},
		{1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
			2.96460137564761618e-03 * daysPerYear, 2.37847173959480950e-03 * daysPerYear,
			-2.96589568540237556e-05 * daysPerYear, 4.36624404335156298e-05 * solarMass},
		{1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
			2.68067772490389322e-03 * daysPerYear, 1.62824170038242295e-03 * daysPerYear,
			-9.51592254519715870e-05 * daysPerYear, 5.15138902046611451e-05 * solarMass},
	}
	var px, py, pz float64
	for _, b := range bodies {
		px += b.vx * b.mass
		py += b.vy * b.mass
		pz += b.vz * b.mass
	}
	bodies[0].vx = -px / solarMass
	bodies[0].vy = -py / solarMass
	bodies[0].vz = -pz / solarMass
	fmt.Println(energy(bodies))
	for i := 0; i < 50000000; i++ {
		advance(bodies, 0.01)
	}
	fmt.Println(energy(bodies))
}

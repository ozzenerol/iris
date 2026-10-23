package main

import "fmt"

const rounds = 2000000
const workers = 1000
const jobsN = 5000000

func pong(ping, back chan int64) {
	for {
		v := <-ping
		back <- v + 1
		if v < 0 {
			return
		}
	}
}

func worker(jobs, results chan int64) {
	var sum int64
	for {
		v := <-jobs
		if v < 0 {
			results <- sum
			return
		}
		sum += v
	}
}

func main() {
	ping := make(chan int64)
	back := make(chan int64)
	go pong(ping, back)
	var v int64
	for i := 0; i < rounds; i++ {
		ping <- v
		v = <-back
	}
	ping <- -1
	<-back

	jobs := make(chan int64, 1024)
	results := make(chan int64, workers)
	for i := 0; i < workers; i++ {
		go worker(jobs, results)
	}
	for i := int64(0); i < jobsN; i++ {
		jobs <- i
	}
	for i := 0; i < workers; i++ {
		jobs <- -1
	}
	var total int64
	for i := 0; i < workers; i++ {
		total += <-results
	}
	fmt.Println(v, total)
}

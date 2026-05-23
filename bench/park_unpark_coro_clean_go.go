// Go equivalent of park_unpark_coro_clean.glide. Two goroutines
// ping-pong an int via cap-1 chans; main blocks on done chan for
// elapsed-time reading. The hot loop never touches main, so the
// number measures pure goroutine-to-goroutine park/unpark.

package main

import (
	"fmt"
	"time"
)

const N = 100000
const SENTINEL = -1

func pinger(rx <-chan int, tx chan<- int, done chan<- int64) {
	// Warm-up.
	for w := 0; w < 100; w++ {
		tx <- 0
		<-rx
	}
	t0 := time.Now()
	for i := 0; i < N; i++ {
		tx <- i
		<-rx
	}
	elapsed := time.Since(t0).Nanoseconds()
	tx <- SENTINEL
	done <- elapsed
}

func ponger(rx <-chan int, tx chan<- int) {
	for {
		v := <-rx
		if v == SENTINEL {
			return
		}
		tx <- v + 1
	}
}

func main() {
	toPong := make(chan int, 1)
	fromPong := make(chan int, 1)
	done := make(chan int64, 1)

	go pinger(fromPong, toPong, done)
	go ponger(toPong, fromPong)

	elapsed := <-done
	perCycle := elapsed / int64(N)
	fmt.Printf("coro-coro park-unpark %d cycles total_ns: %d avg_ns/cycle: %d\n",
		N, elapsed, perCycle)
}

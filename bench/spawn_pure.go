package main

import (
	"fmt"
	"sync/atomic"
	"time"
)

var pending int32

func worker() {
	atomic.AddInt32(&pending, -1)
}

func main() {
	const n = 100000
	atomic.StoreInt32(&pending, int32(n))
	t0 := time.Now()
	for i := 0; i < n; i++ {
		go worker()
	}
	for atomic.LoadInt32(&pending) > 0 {
	}
	d := time.Since(t0)
	fmt.Printf("spawn+drain %d: %d ms\n", n, d.Milliseconds())
}

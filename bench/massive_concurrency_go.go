// Go equivalent of massive_concurrency.glide. Spawn N goroutines all
// parked on the same chan recv that will never fire; measure RSS via
// runtime.MemStats after a settle delay. Reports bytes-per-goroutine
// as the headline metric.
//
// Override the count with GLIDE_MASSIVE_N=<int>; default 100k so the
// bench finishes quickly on a stock dev box.

package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
)

func parker(c chan int) {
	<-c // blocks forever
}

func main() {
	n := 100000
	if env := os.Getenv("GLIDE_MASSIVE_N"); env != "" {
		if v, err := strconv.Atoi(env); err == nil {
			n = v
		}
	}

	var msBefore runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&msBefore)
	baseline := msBefore.Sys

	t0 := time.Now()
	c := make(chan int, 1)
	for i := 0; i < n; i++ {
		go parker(c)
	}
	// Settle.
	time.Sleep(500 * time.Millisecond)

	var msAfter runtime.MemStats
	runtime.ReadMemStats(&msAfter)
	after := msAfter.Sys

	elapsed := time.Since(t0).Milliseconds()
	delta := int64(after) - int64(baseline)
	perTask := delta / int64(n)
	fmt.Printf("massive %d tasks elapsed_ms: %d rss_baseline_mb: %d "+
		"rss_after_mb: %d delta_mb: %d bytes_per_task: %d\n",
		n, elapsed, baseline/(1024*1024), after/(1024*1024),
		delta/(1024*1024), perTask)
}

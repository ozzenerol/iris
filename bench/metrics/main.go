package main

import (
	"bytes"
	"fmt"
	"strconv"

	"github.com/prometheus/client_golang/prometheus"
	dto "github.com/prometheus/client_model/go"
	"github.com/prometheus/common/expfmt"
)

// 10 million requests counted by method and status, with their time in a
// histogram, then 2,000 series written out 200 times.
func main() {
	reg := prometheus.NewRegistry()
	requests := prometheus.NewCounterVec(prometheus.CounterOpts{Name: "requests_total", Help: "Requests."}, []string{"method", "code"})
	seconds := prometheus.NewHistogram(prometheus.HistogramOpts{Name: "request_seconds", Help: "Request time."})
	total := prometheus.NewCounter(prometheus.CounterOpts{Name: "bytes_total", Help: "Bytes."})
	reg.MustRegister(requests, seconds, total)
	methods := []string{"GET", "POST", "PUT", "DELETE"}
	codes := []string{"200", "201", "404", "500", "503"}
	for i := 0; i < 10000000; i++ {
		requests.WithLabelValues(methods[i%4], codes[i%5]).Inc()
		seconds.Observe(float64(i%1000) / 1000.0)
		total.Add(float64(i % 1500))
	}
	paths := prometheus.NewCounterVec(prometheus.CounterOpts{Name: "path_hits_total", Help: "Hits by path."}, []string{"path"})
	reg.MustRegister(paths)
	for i := 0; i < 2000; i++ {
		paths.WithLabelValues("/items/" + strconv.Itoa(i)).Add(float64(i))
	}
	size := 0
	for i := 0; i < 200; i++ {
		var buf bytes.Buffer
		mfs, _ := reg.Gather()
		for _, mf := range mfs {
			expfmt.MetricFamilyToText(&buf, mf)
		}
		size += buf.Len()
	}
	var m dto.Metric
	requests.WithLabelValues("GET", "200").Write(&m)
	var h dto.Metric
	seconds.Write(&h)
	var t dto.Metric
	total.Write(&t)
	fmt.Println(m.Counter.GetValue(), h.Histogram.GetSampleCount(), t.Counter.GetValue(), size)
}

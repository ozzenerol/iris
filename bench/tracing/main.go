package main

import (
	"encoding/hex"
	"fmt"
	"math/rand/v2"
	"time"

	coltrace "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	common "go.opentelemetry.io/proto/otlp/common/v1"
	resource "go.opentelemetry.io/proto/otlp/resource/v1"
	trace "go.opentelemetry.io/proto/otlp/trace/v1"
	"google.golang.org/protobuf/encoding/protojson"
)

func text(k, v string) *common.KeyValue {
	return &common.KeyValue{Key: k, Value: &common.AnyValue{Value: &common.AnyValue_StringValue{StringValue: v}}}
}

func num(k string, v int64) *common.KeyValue {
	return &common.KeyValue{Key: k, Value: &common.AnyValue{Value: &common.AnyValue_IntValue{IntValue: v}}}
}

func id(n int) []byte {
	b := make([]byte, n)
	for i := 0; i < n; i += 8 {
		v := rand.Uint64()
		for j := 0; j < 8 && i+j < n; j++ {
			b[i+j] = byte(v >> (8 * j))
		}
	}
	return b
}

// 1,000,000 spans in pairs: a root, its traceparent written and read
// back, and a child of it, each with three attributes. Every 512 go out
// as OTLP JSON.
func main() {
	var batch []*trace.Span
	bytes := 0
	spans := 0
	for i := 0; i < 500000; i++ {
		root := &trace.Span{TraceId: id(16), SpanId: id(8), Name: "GET /orders", Kind: trace.Span_SPAN_KIND_SERVER,
			StartTimeUnixNano: uint64(time.Now().UnixNano()), Flags: 257,
			Attributes: []*common.KeyValue{text("http.request.method", "GET"), text("url.path", "/orders"), num("http.response.status_code", 200)}}
		tp := "00-" + hex.EncodeToString(root.TraceId) + "-" + hex.EncodeToString(root.SpanId) + "-01"
		ptrace, _ := hex.DecodeString(tp[3:35])
		pspan, _ := hex.DecodeString(tp[36:52])
		child := &trace.Span{TraceId: ptrace, SpanId: id(8), ParentSpanId: pspan, Name: "SELECT orders", Kind: trace.Span_SPAN_KIND_CLIENT,
			StartTimeUnixNano: uint64(time.Now().UnixNano()), Flags: 257,
			Attributes: []*common.KeyValue{text("db.system", "postgresql"), text("db.statement", "SELECT * FROM orders WHERE id = $1"), num("db.rows", int64(i%100))}}
		child.EndTimeUnixNano = child.StartTimeUnixNano + 1000
		root.EndTimeUnixNano = root.StartTimeUnixNano + 5000
		batch = append(batch, child, root)
		if len(batch) >= 512 {
			spans += len(batch)
			req := &coltrace.ExportTraceServiceRequest{ResourceSpans: []*trace.ResourceSpans{{
				Resource:   &resource.Resource{Attributes: []*common.KeyValue{text("service.name", "shop")}},
				ScopeSpans: []*trace.ScopeSpans{{Scope: &common.InstrumentationScope{Name: "iris"}, Spans: batch}},
			}}}
			out, _ := protojson.Marshal(req)
			bytes += len(out)
			batch = nil
		}
	}
	fmt.Printf("%d spans, %v\n", spans, bytes > 0)
}

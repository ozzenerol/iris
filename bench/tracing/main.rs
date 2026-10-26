use opentelemetry_proto::tonic::collector::trace::v1::ExportTraceServiceRequest;
use opentelemetry_proto::tonic::common::v1::{any_value, AnyValue, InstrumentationScope, KeyValue};
use opentelemetry_proto::tonic::resource::v1::Resource;
use opentelemetry_proto::tonic::trace::v1::{span::SpanKind, ResourceSpans, ScopeSpans, Span};
use rand::RngExt;
use std::fmt::Write;
use std::time::{SystemTime, UNIX_EPOCH};

fn text(k: &str, v: &str) -> KeyValue {
    KeyValue { key: k.into(), value: Some(AnyValue { value: Some(any_value::Value::StringValue(v.into())) }) }
}

fn int(k: &str, v: i64) -> KeyValue {
    KeyValue { key: k.into(), value: Some(AnyValue { value: Some(any_value::Value::IntValue(v)) }) }
}

fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos() as u64
}

fn hex(v: &[u8], out: &mut String) {
    for b in v {
        write!(out, "{b:02x}").unwrap();
    }
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

fn span(trace: Vec<u8>, id: Vec<u8>, parent: Vec<u8>, name: &str, kind: SpanKind, attrs: Vec<KeyValue>) -> Span {
    let start = now();
    Span {
        trace_id: trace,
        span_id: id,
        parent_span_id: parent,
        name: name.into(),
        kind: kind as i32,
        start_time_unix_nano: start,
        attributes: attrs,
        flags: 257,
        ..Default::default()
    }
}

// 1,000,000 spans in pairs: a root, its traceparent written and read
// back, and a child of it, each with three attributes. Every 512 go out
// as OTLP JSON.
fn main() {
    let mut rng = rand::rng();
    let mut batch: Vec<Span> = Vec::new();
    let mut bytes = 0;
    let mut spans = 0;
    for i in 0..500_000i64 {
        let trace: [u8; 16] = rng.random();
        let id: [u8; 8] = rng.random();
        let mut root = span(trace.to_vec(), id.to_vec(), vec![], "GET /orders", SpanKind::Server,
            vec![text("http.request.method", "GET"), text("url.path", "/orders"), int("http.response.status_code", 200)]);
        let mut tp = String::with_capacity(55);
        tp.push_str("00-");
        hex(&root.trace_id, &mut tp);
        tp.push('-');
        hex(&root.span_id, &mut tp);
        tp.push_str("-01");
        let ptrace = unhex(&tp[3..35]).unwrap();
        let pspan = unhex(&tp[36..52]).unwrap();
        let cid: [u8; 8] = rng.random();
        let mut child = span(ptrace, cid.to_vec(), pspan, "SELECT orders", SpanKind::Client,
            vec![text("db.system", "postgresql"), text("db.statement", "SELECT * FROM orders WHERE id = $1"), int("db.rows", i % 100)]);
        child.end_time_unix_nano = child.start_time_unix_nano + 1000;
        root.end_time_unix_nano = root.start_time_unix_nano + 5000;
        batch.push(child);
        batch.push(root);
        if batch.len() >= 512 {
            spans += batch.len();
            let req = ExportTraceServiceRequest {
                resource_spans: vec![ResourceSpans {
                    resource: Some(Resource { attributes: vec![text("service.name", "shop")], ..Default::default() }),
                    scope_spans: vec![ScopeSpans {
                        scope: Some(InstrumentationScope { name: "iris".into(), ..Default::default() }),
                        spans: std::mem::take(&mut batch),
                        ..Default::default()
                    }],
                    ..Default::default()
                }],
            };
            bytes += serde_json::to_vec(&req).unwrap().len();
        }
    }
    println!("{} spans, {}", spans, bytes > 0);
}

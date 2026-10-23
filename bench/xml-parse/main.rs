use quick_xml::events::Event;
use quick_xml::Reader;
use std::fmt::Write;

fn next(s: &mut i64) -> i64 {
    *s = (*s * 1103515245 + 12345) % 2147483648;
    *s
}

// A 30 MB document of items made in memory, read as events: elements,
// attributes and bytes of text.
fn main() {
    let mut doc = String::from("<catalog>");
    let mut seed = 42i64;
    let mut i = 0i64;
    while doc.len() < 30000000 {
        let r = next(&mut seed);
        write!(doc, "<item id=\"{}\" kind=\"k{}\"><name>Item {} &amp; co</name><price>{}</price><tags><tag>a{}</tag><tag>b&lt;{}&gt;</tag></tags></item>", i, r % 7, i, r % 10000, r % 13, r % 5).unwrap();
        i += 1;
    }
    doc.push_str("</catalog>");
    let mut rd = Reader::from_str(&doc);
    let (mut elements, mut attrs, mut text) = (0usize, 0usize, 0usize);
    loop {
        match rd.read_event().unwrap() {
            Event::Start(e) => {
                elements += 1;
                for a in e.attributes() {
                    a.unwrap().unescape_value().unwrap();
                    attrs += 1;
                }
            }
            Event::Empty(e) => {
                elements += 1;
                attrs += e.attributes().count();
            }
            Event::Text(t) => text += t.decode().unwrap().len(),
            Event::GeneralRef(r) => {
                text += r.resolve_char_ref().unwrap().map(|c| c.len_utf8()).unwrap_or_else(|| {
                    match &*r.decode().unwrap() {
                        "amp" | "lt" | "gt" | "quot" | "apos" => 1,
                        _ => panic!("unknown entity"),
                    }
                })
            }
            Event::Eof => break,
            _ => {}
        }
    }
    println!("{} {} {}", elements, attrs, text);
}

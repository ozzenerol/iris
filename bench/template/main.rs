use minijinja::{context, Environment};
use serde::Serialize;

#[derive(Serialize)]
struct Row {
    id: i64,
    name: String,
    qty: i64,
}

// A page with a table of 1000 rows whose text needs escaping, rendered
// 2000 times.
fn main() {
    let mut env = Environment::new();
    env.set_auto_escape_callback(|_| minijinja::AutoEscape::Html);
    env.add_template("page", "<html><head><title>{{ title }}</title></head><body><table>{% for r in rows %}<tr><td>{{ r.id }}</td><td>{{ r.name }}</td><td>{{ r.qty }}</td></tr>{% endfor %}</table></body></html>").unwrap();
    let t = env.get_template("page").unwrap();
    let rows: Vec<Row> = (0..1000).map(|i| Row { id: i, name: format!("item <{}> & more", i), qty: i % 17 }).collect();
    let mut total = 0;
    for _ in 0..2000 {
        let html = t.render(context! { title => "Orders & <returns>", rows => &rows }).unwrap();
        total += html.len();
    }
    println!("{}", total);
}

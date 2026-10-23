# Templates

A template is text with places for values: an HTML page, an email, a config file for another program. The `template` module reads a template once and fills it in from a `@json` struct or a `Map` as often as needed. Values are escaped for HTML unless the template says otherwise, so text from users can't add tags to a page.

## Filling in values

`template.parse(text)` reads a template into a `template.Template`, or fails with `TemplateError.syntax`. `render(data)` fills it from a `@json` struct and gives the text. `{{ name }}` writes a field, and `{{ user.name }}` a field of a struct inside it. Templates are written in raw strings, where braces are plain characters:

```iris run
import template

@json
struct User {
    String name
    String email
}

@json
struct Page {
    String title
    User user
    i64 unread
}

!void main() {
    scoped template.Template t = try template.parse(r"""<h1>{{ title }}</h1>
<p>Signed in as {{ user.name }} ({{ user.email }}), {{ unread }} unread.</p>""")
    scoped Page p = Page{title: "Inbox".copy(), user: User{name: "Ada <admin>".copy(), email: "ada@example.com".copy()}, unread: 3}
    scoped String html = try t.render(p)
    print(html)
}
```

```output
<h1>Inbox</h1>
<p>Signed in as Ada &lt;admin&gt; (ada@example.com), 3 unread.</p>
```

The `<` and `>` in the name came out as `&lt;` and `&gt;`, so the browser shows them as text. `{{ raw name }}` writes a value as it is, for HTML the program made itself and trusts.

A template is parsed once, at the start, and rendered for every request. `render` doesn't change it, so one template can serve many tasks and threads.

## Conditions

`{{ if name }}` keeps a part when the value is true, not zero, and not empty, and `{{ else }}` and `{{ else if other }}` give the other cases. `{{ if not name }}` turns the test around, and `==` and `!=` compare with text, a number or another value:

```iris run
import template

enum State { running, stopped, failed }

@json
struct Service {
    String name
    State state
    i64 restarts
}

!void main() {
    scoped template.Template t = try template.parse(r"""{{ name }}: {{ if state == "running" }}up{{ else if state == "failed" }}FAILED{{ else }}stopped{{ end }}{{ if restarts }}, restarted {{ restarts }} times{{ end }}""")
    for s in [Service{name: "api".copy(), state: .running}, Service{name: "worker".copy(), state: .failed, restarts: 4}, Service{name: "cron".copy(), state: .stopped}] {
        scoped String line = try t.render(s)
        print(line)
        s.free()
    }
}
```

```output
api: up
worker: FAILED, restarted 4 times
cron: stopped
```

An enum is compared by its value's name.

## Loops

`{{ for item in list }}` repeats a part for each value of a list, and `{{ for i, item in list }}` gives its place too, from 0. An `{{ else }}` inside the loop is used when the list is empty, and `{{ list.len }}` is how many there are. A block tag alone on its line takes the line with it, so the output has no blank lines where the tags were:

```iris run
import template

@json
struct Order {
    i64 id
    String customer
    f64 total
}

@json
struct Report {
    List[Order] orders
}

!void main() {
    scoped template.Template t = try template.parse(r"""
<table>
  {{ for i, o in orders }}
  <tr><td>{{ i }}</td><td>#{{ o.id }}</td><td>{{ o.customer }}</td><td>{{ o.total }}</td></tr>
  {{ else }}
  <tr><td colspan="4">no orders today</td></tr>
  {{ end }}
</table>
<p>{{ orders.len }} orders</p>
""")
    scoped Report r
    r.orders.push(Order{id: 1041, customer: "Ada".copy(), total: 42.5})
    r.orders.push(Order{id: 1042, customer: "Linus".copy(), total: 9.99})
    scoped String full = try t.render(r)
    print(full)
    scoped Report none
    scoped String empty = try t.render(none)
    print(empty)
}
```

```output
<table>
  <tr><td>0</td><td>#1041</td><td>Ada</td><td>42.5</td></tr>
  <tr><td>1</td><td>#1042</td><td>Linus</td><td>9.99</td></tr>
</table>
<p>2 orders</p>
<table>
  <tr><td colspan="4">no orders today</td></tr>
</table>
<p>0 orders</p>
```

`{{# a comment }}` is left out of the output.

## Data from a Map

Templates aren't only for HTML. When the values don't fit a struct, `renderMap(map)` fills the template from a `Map` with `str` keys. This writes a systemd unit for each service a program deploys:

```iris run
import template

!void main() {
    scoped template.Template t = try template.parse(r"""[Unit]
Description={{ name }}

[Service]
ExecStart=/srv/{{ name }}/bin/{{ name }} --port {{ port }}
User={{ user }}""")
    Map[str, str] vars
    defer vars.free()
    vars.set("name", "shop")
    vars.set("port", "8080")
    vars.set("user", "shop")
    scoped String unit = try t.renderMap(vars)
    print(unit)
}
```

```output
[Unit]
Description=shop

[Service]
ExecStart=/srv/shop/bin/shop --port 8080
User=shop
```

Escaping for HTML is harmless here, since the values have no `&`, `<`, `>` or quotes. For text that may have them, write `{{ raw name }}`.

## Pages made of parts

`add(name, text)` adds another template, and `{{ include "name" }}` puts it in, filled from the same data. Parts that every page has, such as a header and a footer, are written once:

```iris run
import template

@json
struct Page {
    String title
    String user
}

!void main() {
    scoped template.Template t = try template.parse(r"""{{ include "header" }}
<main>Welcome back.</main>
{{ include "footer" }}""")
    try t.add("header", r"""<header><title>{{ title }}</title>{{ if user }}Hi, {{ user }}{{ else }}<a href="/login">Sign in</a>{{ end }}</header>""")
    try t.add("footer", r"""<footer>&copy; 2026</footer>""")
    scoped Page p = Page{title: "Shop".copy(), user: "Ada".copy()}
    scoped String html = try t.render(p)
    print(html)
    scoped String header = try t.render(p, "header")
    print(header)
}
```

```output
<header><title>Shop</title>Hi, Ada</header>
<main>Welcome back.</main>
<footer>&copy; 2026</footer>
<header><title>Shop</title>Hi, Ada</header>
```

The second argument of `render` names the template to fill, the main one when it is left out.

A site keeps its templates in files. `template.load(dir, main)` reads every file in a folder, each named by its path inside it, and `main` is the one `render` fills:

```iris run
import fs, template

@json
struct Page {
    String title
}

!void main() {
    dir := "/tmp/iris-tour-templates"
    try fs.mkdirAll("{dir}/parts")
    try fs.writeAll("{dir}/parts/nav.html", "<nav>Home | Docs</nav>")
    try fs.writeAll("{dir}/index.html", r"""{{ include "parts/nav.html" }}<h1>{{ title }}</h1>""")
    scoped template.Template t = try template.load(dir, "index.html")
    scoped Page p = Page{title: "Docs".copy()}
    scoped String html = try t.render(p)
    print(html)
    try fs.removeAll(dir)
}
```

```output
<nav>Home | Docs</nav><h1>Docs</h1>
```

## Mistakes in a template

A tag that can't be read fails in `parse` with `TemplateError.syntax`. A name the data doesn't have fails in `render` with `TemplateError.missing`, with the line, so a misspelled name shows up the first time the page is made. Inside `{{ if }}` a missing name counts as false instead:

```iris run
import template

@json
struct Page {
    String title
}

!void main() {
    _ = template.parse("{{ if title }}no end") catch e {
        print("{e.code}: {e.message}")
        template.Template{}
    }
    scoped template.Template t = try template.parse("<h1>{{ title }}</h1>\n<p>{{ subtitle }}</p>")
    scoped Page p = Page{title: "Shop".copy()}
    _ = t.render(p) catch e {
        print("{e.code}: {e.message}")
        "".copy()
    }
}
```

```output
TemplateError.syntax: line 1: {{ if }} has no {{ end }}
TemplateError.missing: line 2: subtitle is not in the data
```

## Serving pages

A web server parses its templates in `main`, keeps them where its handlers can reach them, and renders one for each request. `res.html` answers with the page and sets the `Content-Type` to HTML (see [HTTP](14-http.html)):

```iris run
import http, net, template

@json
struct Hello {
    String name
}

template.Template page

!void hello(http.Request *req, http.Response *res) {
    name := req.query("name")
    scoped Hello h = Hello{name: (if name.len > 0 { name } else { "stranger" }).copy()}
    scoped String html = try page.render(h)
    res.html(html)
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    page = try template.parse(r"""<p>Hello, {{ name }}!</p>""")
    defer page.free()
    http.Router r
    defer r.free()
    r.get("/", hello)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    for query in ["", "?name=Ada", "?name=<b>Ada</b>"] {
        scoped String url = format("http://127.0.0.1:{ln.port}/{query}")
        scoped http.Reply reply = try http.get(url)
        print("{reply.header("Content-Type")}: {reply.body}")
    }
}
```

```output
text/html; charset=utf-8: <p>Hello, stranger!</p>
text/html; charset=utf-8: <p>Hello, Ada!</p>
text/html; charset=utf-8: <p>Hello, &lt;b&gt;Ada&lt;/b&gt;!</p>
```

The tags in the last name came out as text, so a visitor can't add HTML to the page. A browser sends `<` as `%3C`, and `req.query` gives it back as `<`, so the escaping is what keeps the page safe.

`template.escape(text)` escapes any text for HTML the same way, for the rare page built without a template.

## Filling in a template

`template.parse(text)` reads a template once; `render(data)` fills it in from a `@json` struct, as often as needed. `{{ name }}` writes a value, escaped for HTML, and `{{ user.name }}` a field of a struct inside it. `{{ raw name }}` writes a value as it is, for HTML the program made itself. Templates are best written in raw strings, `r"..."` or `r"""..."""`, where braces are plain characters:

```iris run
import template

@json
struct Item {
    String name
    f64 price
}

@json
struct Page {
    String shop
    bool open
    List[Item] items
}

!void main() {
    scoped template.Template t = try template.parse(r"""
        <h1>{{ shop }}</h1>
        {{ if open }}
        <ul>
          {{ for i, item in items }}
          <li>{{ i }}. {{ item.name }}: {{ item.price }}</li>
          {{ else }}
          <li>nothing yet</li>
          {{ end }}
        </ul>
        {{ else }}
        <p>closed</p>
        {{ end }}
        """)
    Page p = Page{shop: "Tea & <Cake>".copy(), open: true}
    defer p.free()
    p.items.push(Item{name: "green tea".copy(), price: 3.5})
    p.items.push(Item{name: "lemon cake".copy(), price: 4.25})
    scoped String html = try t.render(p)
    print(html)
}
```

```output
<h1>Tea &amp; &lt;Cake&gt;</h1>
<ul>
  <li>0. green tea: 3.5</li>
  <li>1. lemon cake: 4.25</li>
</ul>

```

A block tag (`{{ if }}`, `{{ else }}`, `{{ end }}`, `{{ for }}` or a `{{# comment }}`) alone on its line takes the line out with it, so the output has no blank lines where they were.

## What a template can say

| Tag | What it does |
|---|---|
| `{{ name }}`, `{{ a.b.c }}` | the value, escaped for HTML |
| `{{ raw name }}` | the value as it is |
| `{{ list.len }}` | how many values a list holds, or bytes a text |
| `{{ if name }}` … `{{ else if other }}` … `{{ else }}` … `{{ end }}` | a part kept when the value is true, not zero, not empty |
| `{{ if not name }}` | the other way around |
| `{{ if state == "open" }}`, `{{ if n != 0 }}` | compares with text, a number, `true`, `false` or another value |
| `{{ for x in list }}` … `{{ else }}` … `{{ end }}` | the part once for each value, or the else part when there are none |
| `{{ for i, x in list }}` | with the place of each, from 0; over a struct, each field's name and value |
| `{{ include "name" }}` | another template, filled from the same data |
| `{{# a comment }}` | nothing |

A value that the data doesn't have fails with `TemplateError.missing`, with the line, so a misspelled name shows up; in `{{ if }}` it counts as false. A list or a struct written as text fails with `TemplateError.wrongType`.

## A Map and other templates

`renderMap(map)` fills a template from a `Map` with `str` keys, whose values can be text, numbers, lists or `@json` structs. `add(name, text)` adds a template that `{{ include "name" }}` puts in, and `template.load(dir, main)` reads every file in a folder as a template named by its path inside it, with `main` as the one `render` fills:

```iris run
import template

!void main() {
    scoped template.Template t = try template.parse(r"""{{ include "header" }}<p>{{ count }} new messages</p>""")
    try t.add("header", r"""<header>Hello, {{ user }}</header>""")
    Map[str, str] data
    defer data.free()
    data.set("user", "Ada")
    data.set("count", "3")
    scoped String html = try t.renderMap(data)
    print(html)
    scoped String header = try t.renderMap(data, "header")
    print(header)
}
```

```output
<header>Hello, Ada</header><p>3 new messages</p>
<header>Hello, Ada</header>
```

`template.escape(text)` escapes text for HTML the same way: `&`, `<`, `>`, `"` and `'`.

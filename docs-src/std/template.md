## Filling in a template

`template.parse(text)` reads a template once, and `render(data)` fills it in from a `@json` struct as often as needed.

- `{{ name }}` writes a value, escaped for HTML.
- `{{ user.name }}` writes a field of a struct inside the data.
- `{{ raw name }}` writes a value as it is, for HTML the program made itself.

Write templates in raw strings, `r"..."` or `r"""..."""`, where braces are plain characters:

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

A block tag (`{{ if }}`, `{{ else }}`, `{{ end }}`, `{{ for }}` or `{{# comment }}`) alone on its line removes the whole line, so the output has no blank lines where the tags were.

## What a template can say

| Tag | What it does |
|---|---|
| `{{ name }}`, `{{ a.b.c }}` | The value, escaped for HTML. |
| `{{ raw name }}` | The value as it is. |
| `{{ list.len }}` | The number of values in a list, or of bytes in a text. |
| `{{ if name }}` … `{{ else if other }}` … `{{ else }}` … `{{ end }}` | A part kept when the value is true, not zero and not empty. |
| `{{ if not name }}` | The opposite test. |
| `{{ if state == "open" }}`, `{{ if n != 0 }}` | Compares with text, a number, `true`, `false` or another value. |
| `{{ for x in list }}` … `{{ else }}` … `{{ end }}` | The part once for each value, or the else part when there are none. |
| `{{ for i, x in list }}` | Also gives the place of each value, from 0. Over a struct, each field's name and value. |
| `{{ include "name" }}` | Another template, filled from the same data. |
| `{{# a comment }}` | Nothing. |

A value missing from the data fails with `TemplateError.missing` and the line, so a misspelled name shows up. Inside `{{ if }}` a missing value counts as false. A list or a struct written as text fails with `TemplateError.wrongType`.

## A Map and other templates

`renderMap(map)` fills a template from a `Map` with `str` keys. Its values can be text, numbers, lists or `@json` structs. `add(name, text)` adds a template for `{{ include "name" }}`. `template.load(dir, main)` reads every file in a folder as a template named by its path inside the folder, and `render` fills the one named `main`:

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

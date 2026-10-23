## A tree

`xml.parse(text)` reads a document into its root `xml.Element`. An element has a `name`, its `attrs`, its `children` and its `text`, the text directly inside it. `child(name)` gives the first child element called name, or `null`, and `attr(name)` the value of an attribute, `""` when it isn't there. Text that is only spaces and line breaks is left out, and entities such as `&amp;` are decoded:

```iris run
import xml

!void main() {
    text := r"""
        <?xml version="1.0"?>
        <catalog>
          <book id="bk101" lang="en">
            <title>XML &amp; you</title>
            <price>44.95</price>
          </book>
          <book id="bk102">
            <title>Midnight Rain</title>
            <price>5.95</price>
          </book>
        </catalog>
        """
    scoped xml.Element root = try xml.parse(text)
    for b in root.children {
        print("{b.attr("id")}: {b.child("title").text}, {b.child("price").text} [{b.attr("lang")}]")
    }
}
```

```output
bk101: XML & you, 44.95 [en]
bk102: Midnight Rain, 5.95 []
```

A text child has no name: `isText()` tells the two apart, and `allText()` joins all the text inside an element and the ones in it. Comments, processing instructions and the `DOCTYPE` line are skipped; DTDs are not read, and a name with a prefix, like `atom:id`, is kept as it is.

## Events

`xml.reader(text)` reads a document a step at a time, which suits large ones: each `next` gives an `xml.Event`, a `.start` with its `name` and `attrs`, a `.text` with its `text`, or an `.end`. An empty tag such as `<br/>` gives a start and an end. At the end of the document `next` fails with `XmlError.end`:

```iris run
import xml

!void main() {
    rd := xml.reader(r"""<log><line level="warn">disk at 91%</line><line level="info">ok</line><sync/></log>""")
    for {
        ev := rd.next() catch e {
            if e.code != xml.XmlError.end {
                print("{e.code}: {e.message}")
            }
            break
        }
        match ev.kind {
            .start => print("start {ev.name} [{ev.attr("level")}]")
            .text => print("  {ev.text}")
            .end => print("end {ev.name}")
        }
        ev.free()
    }
}
```

```output
start log []
start line [warn]
  disk at 91%
end line
start line [info]
  ok
end line
start sync []
end sync
end log
```

Text that isn't well-formed fails with `XmlError.syntax` and its line: a tag closed by the wrong end tag, one never closed, an attribute without quotes, an entity XML doesn't have, or a second root element.

## Writing

`xml.element(name)` makes an element; `set` gives it an attribute, `add` a child (which it owns after) and `addText` text. `write` gives it as XML on one line, with `&`, `<`, `>`, `"` and `'` written as entities. `xml.escape(text)` does that to any text:

```iris run
import xml

void main() {
    scoped xml.Element order = xml.element("order")
    order.set("id", "7")
    item := xml.element("item")
    item.set("qty", "2")
    item.addText("tea & <cake>")
    order.add(item)
    order.add(xml.element("gift"))
    scoped String text = order.write()
    print(text)
}
```

```output
<order id="7"><item qty="2">tea &amp; &lt;cake&gt;</item><gift/></order>
```

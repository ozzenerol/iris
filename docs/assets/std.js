/* The library reference, as rustdoc shows a crate: #module is a module's
   page, #module/name one of its items, nothing the library's front page.
   It fills the sidebar's middle and the main content.
   Data: window.IRIS_STD (std/data.js). */
(function () {
  var data = window.IRIS_STD;
  var mods = data.modules;
  var main = document.getElementById("std-main");
  var side = document.getElementById("std-side");
  var GITHUB = "https://github.com/ozzenerol/iris/blob/main/std/";

  /* rustdoc's sections of a module, in its order: [kind, heading, what an item is called, its colour class] */
  var KINDS = [["struct", "Structs", "Struct", "struct"], ["enum", "Enums", "Enum", "enum"],
               ["interface", "Interfaces", "Interface", "interface"], ["type", "Type Aliases", "Type", "type"],
               ["func", "Functions", "Function", "fn"], ["builtin", "Builtins", "Builtin", "builtin"],
               ["method", "Methods", "Method", "method"], ["const", "Constants", "Constant", "constant"],
               ["var", "Variables", "Variable", "static"]];
  function kindOf(k) {
    for (var i = 0; i < KINDS.length; i++) if (KINDS[i][0] === k) return KINDS[i];
    return [k, k, k, "fn"];
  }

  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  var KW = /^(if|else|for|in|match|break|continue|return|struct|union|enum|interface|type|dyn|pub|const|comptime|when|import|try|catch|defer|errdefer|scoped|spawn|select|chan|thread|threadlocal|asm|test|not|and|or|is|true|false|null|self)$/;
  var TY = /^(i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|bool|void|str|String|Error|Duration|List|Map|Set|Arena|Atomic|Mutex|Thread|u16be|u32be|u64be|i16be|i32be|i64be)$/;
  function span(cls, t) { return '<span class="' + cls + '">' + esc(t) + "</span>"; }

  /* where the string starting at code[i] ends; braces hold code, which may
     have strings of its own */
  function stringEnd(code, i) {
    var triple = code.substr(i, 3) === '"""';
    i += triple ? 3 : 1;
    var depth = 0;
    while (i < code.length) {
      var ch = code[i];
      if (ch === "\\") { i += 2; continue; }
      if (depth === 0) {
        if (triple && code.substr(i, 3) === '"""') return i + 3;
        if (!triple && ch === '"') return i + 1;
        if (!triple && ch === "\n") return i;
        if (code.substr(i, 2) === "{{" || code.substr(i, 2) === "}}") { i += 2; continue; }
      }
      if (ch === "{") depth++;
      else if (ch === "}" && depth > 0) depth--;
      else if (ch === '"' && depth > 0) { i = stringEnd(code, i); continue; }
      i++;
    }
    return i;
  }

  /* a string: escapes and the {} around values stand out, and the code in
     the braces is highlighted as code */
  function stringHtml(t) {
    var q = t.substr(0, 3) === '"""' ? 3 : 1;
    var close = q === 3 ? '"""' : '"';
    var bodyEnd = t.length >= 2 * q && t.substr(t.length - q) === close ? t.length - q : t.length;
    var out = "", plain = t.slice(0, q), i = q, m;
    var escRe = /\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]+\}|.)/y;
    function flush() { if (plain) { out += span("s", plain); plain = ""; } }
    while (i < bodyEnd) {
      var two = t.substr(i, 2);
      if (two === "{{" || two === "}}") { flush(); out += span("e", two); i += 2; continue; }
      escRe.lastIndex = i;
      if (t[i] === "\\" && (m = escRe.exec(t))) { flush(); out += span("e", m[0]); i = escRe.lastIndex; continue; }
      if (t[i] === "{") {
        var j = i + 1, depth = 1;
        while (j < bodyEnd && depth) {
          if (t[j] === '"') { j = stringEnd(t, j); continue; }
          if (t[j] === "{") depth++;
          else if (t[j] === "}") depth--;
          j++;
        }
        flush();
        out += span("e", "{") + hl(t.slice(i + 1, depth === 0 ? j - 1 : j));
        if (depth === 0) out += span("e", "}");
        i = j;
        continue;
      }
      plain += t[i];
      i++;
    }
    plain += t.slice(bodyEnd);
    flush();
    return out;
  }

  function hl(code) {
    var re = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|(r"""[\s\S]*?"""|r"[^"\n]*")|(")|('(?:\\.|[^'\\\n])+')|(@[A-Za-z_]\w*)|(\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)(?:ns|us|ms|s|m|h|KiB|MiB|GiB)?\b)|([A-Za-z_]\w*)/g;
    var out = "", last = 0, m;
    while ((m = re.exec(code))) {
      out += esc(code.slice(last, m.index));
      var t = m[0];
      if (m[1]) out += span("c", t);
      else if (m[2] || m[4]) out += span("s", t);
      else if (m[3]) {
        var end = stringEnd(code, m.index);
        out += stringHtml(code.slice(m.index, end));
        re.lastIndex = end;
      }
      else if (m[5]) out += span("a", t);
      else if (m[6]) out += span("n", t);
      else if (KW.test(t)) out += span("k", t);
      else if (TY.test(t)) out += span("t", t);
      else if (code[re.lastIndex] === "(") out += span("f", t);
      else if (/^[A-Z]/.test(t) && /[a-z]/.test(t)) out += span("t", t);
      else out += esc(t);
      last = re.lastIndex;
    }
    return out + esc(code.slice(last));
  }

  /* a doc comment: paragraphs; lines indented by 2+ spaces are code; `x` is code */
  function inline(s) {
    return esc(s).replace(/`([^`]+)`/g, "<code>$1</code>");
  }
  function docHtml(text) {
    if (!text) return "";
    var out = [], para = [], code = [];
    function flushPara() { if (para.length) { out.push("<p>" + inline(para.join(" ")) + "</p>"); para = []; } }
    function flushCode() {
      if (code.length) {
        var min = Math.min.apply(null, code.filter(function (l) { return l.trim(); }).map(function (l) { return l.match(/^ */)[0].length; }));
        out.push('<pre class="code"><code>' + hl(code.map(function (l) { return l.slice(min); }).join("\n").replace(/\s+$/, "")) + "</code></pre>");
        code = [];
      }
    }
    text.split("\n").forEach(function (line) {
      if (/^ {2,}\S/.test(line) || (code.length && /^\s*$/.test(line))) { flushPara(); code.push(line); }
      else if (!line.trim()) { flushCode(); flushPara(); }
      else { flushCode(); para.push(line.trim()); }
    });
    flushCode(); flushPara();
    return out.join("");
  }
  function firstSentence(text) {
    if (!text) return "";
    var s = text.split("\n\n")[0].replace(/\s+/g, " ").trim();
    var m = s.match(/^(.+?\.)(\s|$)/);
    s = m ? m[1] : s;
    s = s.replace(/:$/, "");
    return s.length > 140 ? s.slice(0, 137) + "..." : s;
  }

  function modByName(name) { for (var i = 0; i < mods.length; i++) if (mods[i].name === name) return mods[i]; return null; }
  function href(m, d) { return "#" + m.name + (d ? "/" + d.name : ""); }
  function link(m, d, text) {
    return '<a class="' + kindOf(d ? d.kind : "mod")[3] + '" href="' + href(m, d) + '">' + esc(text) + "</a>";
  }
  function modLink(m) { return '<a class="mod" href="' + href(m) + '">' + esc(m.name) + "</a>"; }

  /* the type a method belongs to: File for File.read */
  function owner(d) { var p = d.name.split("."); return p.length > 1 ? p[p.length - 2] : ""; }
  function typeNames(m) {
    var t = {};
    m.decls.forEach(function (d) { if (d.kind === "struct" || d.kind === "enum" || d.kind === "interface" || d.kind === "type") t[d.name] = true; });
    return t;
  }
  /* a module's items by section; methods of its own types go on the type's page */
  function sections(m) {
    var own = typeNames(m), g = {};
    m.decls.forEach(function (d) {
      if (d.kind === "method" && own[owner(d)]) return;
      (g[d.kind] = g[d.kind] || []).push(d);
    });
    return g;
  }
  function methodsOf(m, type) {
    var out = [];
    mods.forEach(function (mm) {
      if (mm !== m && m.name !== "core") return;
      mm.decls.forEach(function (x) { if (x.kind === "method" && owner(x) === type) out.push([mm, x]); });
    });
    return out;
  }

  function heading(crumbs, h1, oob) {
    return '<div class="main-heading">' + (crumbs ? '<div class="rustdoc-breadcrumbs">' + crumbs + "</div>" : "") +
      "<h1>" + h1 + '</h1><span class="out-of-band">' + oob + "</span></div>";
  }
  function sectionHeader(id, text) {
    return '<h2 id="' + id + '" class="section-header">' + esc(text) + '<a href="' + location.pathname + "#" + id + '" class="anchor">&sect;</a></h2>';
  }
  function itemTable(rows) {
    return '<dl class="item-table">' + rows.map(function (r) { return "<dt>" + r[0] + "</dt><dd>" + r[1] + "</dd>"; }).join("") + "</dl>";
  }
  function src(d) { return '<a class="src" href="' + GITHUB + d.file + "#L" + d.line + '">Source</a>'; }

  /* ---- the sidebar ---- */
  function block(title, items) {
    return "<h3>" + esc(title) + '</h3><ul class="block">' + items.join("") + "</ul>";
  }
  function li(h, text, on) { return '<li><a href="' + h + '"' + (on ? ' class="current"' : "") + ">" + esc(text) + "</a></li>"; }
  function modulesBlocks(cur) {
    var out = "", group = null, items = [];
    mods.forEach(function (m) {
      if (m.group !== group) { if (items.length) out += block(group, items); group = m.group; items = []; }
      items.push(li(href(m), m.name, m === cur));
    });
    return out + (items.length ? block(group, items) : "");
  }
  function itemsBlocks(m, cur) {
    var g = sections(m), out = "";
    KINDS.forEach(function (k) {
      if (!g[k[0]]) return;
      out += block(k[1], g[k[0]].map(function (d) { return li(href(m, d), d.name, d === cur); }));
    });
    return out;
  }
  function renderSide(m, d) {
    var out;
    if (!m) {
      out = '<h2 class="location"><a href="#">Library</a></h2><div class="sidebar-elems-inner">' + modulesBlocks(null) + "</div>";
    } else if (!d) {
      out = '<h2 class="location"><a href="' + href(m) + '">Module ' + esc(m.name) + "</a></h2>" +
        '<div class="sidebar-elems-inner">' + itemsBlocks(m, null) +
        '<h2 class="in-crate"><a href="#">In the library</a></h2>' + modulesBlocks(m) + "</div>";
    } else {
      var k = kindOf(d.kind), inner = "";
      if (d.kind === "struct" || d.kind === "enum" || d.kind === "interface") {
        var ms = methodsOf(m, d.name);
        if (ms.length) inner += block("Methods", ms.map(function (p) {
          return li(href(p[0], p[1]), p[1].name.split(".").pop(), false);
        }));
      }
      out = '<h2 class="location"><a href="' + href(m, d) + '">' + esc(k[2] + " " + d.name) + "</a></h2>" +
        '<div class="sidebar-elems-inner">' + inner +
        '<h2 class="in-crate"><a href="' + href(m) + '">In ' + esc(m.name) + "</a></h2>" + itemsBlocks(m, d) + "</div>";
    }
    side.innerHTML = out;
  }

  /* ---- the main content ---- */
  function overview() {
    var html = heading("", 'Library <span class="mod">std</span>', '<span class="since">' + esc(window.IRIS_VERSION || "") + "</span>") +
      '<div class="docblock"><p>Every module of the Iris standard library. A program imports a module (<code>import str</code>) and uses its names with its name in front (<code>str.cut</code>). <code>core</code>, with <code>List</code> and <code>Map</code>, needs no import. Generated from the library\'s source.</p></div>';
    var group = null, rows = [];
    function flush() { if (rows.length) html += sectionHeader(group.toLowerCase().replace(/[^a-z0-9]+/g, "-"), group) + itemTable(rows); rows = []; }
    mods.forEach(function (m) {
      if (m.group !== group) { flush(); group = m.group; }
      rows.push([modLink(m), inline(m.summary)]);
    });
    flush();
    main.innerHTML = html;
  }

  function modulePage(m) {
    var g = sections(m);
    var html = heading('<a href="#">std</a>', 'Module <span class="mod">' + esc(m.name) + "</span>",
      '<a class="src" href="' + GITHUB + m.name + '">Source</a>') +
      '<div class="docblock">' + "<p>" + inline(m.summary) + "</p>" +
      (m.name === "core" ? "" : '<pre class="code"><code>' + hl("import " + m.name) + "</code></pre>") +
      (m.doc ? docHtml(m.doc) : "") + (m.intro || "") + "</div>";
    KINDS.forEach(function (k) {
      if (!g[k[0]]) return;
      html += sectionHeader(k[0] + "s", k[1]) + itemTable(g[k[0]].map(function (d) {
        return [link(m, d, d.name), inline(firstSentence(d.doc))];
      }));
    });
    main.innerHTML = html;
  }

  function declPage(m, d) {
    var k = kindOf(d.kind);
    var name = d.name.split(".").pop();
    var crumbs = '<a href="#">std</a>::<a href="' + href(m) + '">' + esc(m.name) + "</a>";
    if (d.kind === "method") {
      var own = owner(d), t = null;
      mods.forEach(function (mm) { mm.decls.forEach(function (x) { if (!t && x.name === own && x.kind !== "method") t = [mm, x]; }); });
      crumbs += "::" + (t ? link(t[0], t[1], own) : esc(own));
    }
    var oob = (d.platform && d.platform.length ? '<span class="since">' + esc(d.platform.join(", ")) + "</span> &middot; " : "") + src(d);
    var html = heading(crumbs, esc(k[2]) + ' <span class="' + k[3] + '">' + esc(name) + "</span>", oob) +
      '<div class="item-decl"><pre class="code"><code>' + hl(d.sig) + "</code></pre></div>" +
      '<div class="docblock">' + (d.doc ? docHtml(d.doc) : "<p>No description yet.</p>") + "</div>";
    if (d.kind === "struct" || d.kind === "enum" || d.kind === "interface") {
      var ms = methodsOf(m, d.name);
      if (ms.length) {
        html += sectionHeader("methods", "Methods") + '<div class="impl-items">' + ms.map(function (p) {
          var x = p[1], short = x.name.split(".").pop();
          return '<section id="method.' + esc(short) + '"><h4 class="code-header">' +
            hl(x.sig).replace(new RegExp("(\\b" + short.replace(/[^\w]/g, "") + ")(</span>)?\\("), function (all, nm, close) {
              return '<a class="fn" href="' + href(p[0], x) + '">' + nm + "</a>" + (close || "") + "(";
            }) +
            '</h4><div class="docblock">' + docHtml(x.doc) + "</div></section>";
        }).join("") + "</div>";
      }
    }
    main.innerHTML = html;
  }

  function route() {
    var h = decodeURIComponent(location.hash.slice(1));
    var slash = h.indexOf("/");
    var m = modByName(slash >= 0 ? h.slice(0, slash) : h);
    var d = null;
    if (m && slash >= 0) {
      var name = h.slice(slash + 1);
      m.decls.forEach(function (x) { if (x.name === name) d = x; });
    }
    var title;
    if (!m) { overview(); title = "Library reference"; }
    else if (!d) { modulePage(m); title = m.name; }
    else { declPage(m, d); title = d.name + " in " + m.name; }
    document.title = title + " - Iris";
    var mobile = document.querySelector(".mobile-topbar .location");
    if (mobile) mobile.textContent = title;
    renderSide(m, d);
    if (window.irisCopyButtons) window.irisCopyButtons(main);
    if (window.irisCloseNav) window.irisCloseNav();
    var search = document.getElementById("search"), content = document.getElementById("main-content");
    if (search && !search.hidden) { search.hidden = true; content.hidden = false; var i = document.getElementById("site-search"); if (i) i.value = ""; }
    window.scrollTo(0, 0);
  }

  window.addEventListener("hashchange", route);
  route();
})();

/* The library browser: #module shows a module, #module/name one of its
   declarations, nothing the overview. Data: window.IRIS_STD (std/data.js). */
(function () {
  var data = window.IRIS_STD;
  var mods = data.modules;
  var main = document.getElementById("std-main");
  var right = document.getElementById("std-right");
  var tree = document.getElementById("std-modules");
  var found = document.getElementById("std-found");
  var filter = document.getElementById("std-filter");

  var KINDS = [["struct", "Types"], ["enum", "Types"], ["interface", "Types"], ["type", "Types"],
               ["func", "Functions"], ["builtin", "Functions"], ["method", "Methods"], ["const", "Constants"],
               ["var", "Variables"]];
  var GROUP_ORDER = ["Types", "Functions", "Methods", "Constants", "Variables"];
  function groupOf(kind) {
    for (var i = 0; i < KINDS.length; i++) if (KINDS[i][0] === kind) return KINDS[i][1];
    return "Other";
  }

  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  var KW = /^(if|else|for|in|match|break|continue|return|struct|enum|interface|type|dyn|pub|const|comptime|when|import|try|catch|defer|errdefer|scoped|spawn|select|chan|thread|threadlocal|test|true|false|null)$/;
  var TY = /^(i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|bool|void|str|String|Error|Duration|List|Map|Atomic|Mutex)$/;
  function hl(code) {
    var re = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\\n])*")|('(?:\\.|[^'\\\n])+')|(@[A-Za-z_]\w*)|(\b(?:0[xX][0-9a-fA-F_]+|\d[\d_]*(?:\.\d+)?)\b)|([A-Za-z_]\w*)/g;
    var out = "", last = 0, m;
    while ((m = re.exec(code))) {
      out += esc(code.slice(last, m.index));
      var t = esc(m[0]);
      if (m[1]) out += '<span class="c">' + t + "</span>";
      else if (m[2] || m[3]) out += '<span class="s">' + t + "</span>";
      else if (m[4] || m[5]) out += '<span class="n">' + t + "</span>";
      else if (KW.test(m[0])) out += '<span class="k">' + t + "</span>";
      else if (TY.test(m[0])) out += '<span class="t">' + t + "</span>";
      else out += t;
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
  /* a declaration's name in its module: Router.get for a method */
  function shortName(m, d) {
    return d.name;
  }
  function href(m, d) { return "#" + m.name + (d ? "/" + d.name : ""); }

  function grouped(m) {
    var g = {};
    m.decls.forEach(function (d) { var k = groupOf(d.kind); (g[k] = g[k] || []).push(d); });
    return g;
  }

  /* ---- left: the module tree, or what the filter finds ---- */
  function renderTree(cur) {
    var html = "", group = null;
    mods.forEach(function (m) {
      if (m.group !== group) { if (group !== null) html += "</ul>"; group = m.group; html += "<h4>" + esc(group) + "</h4><ul>"; }
      html += '<li><a href="' + href(m) + '"' + (cur === m ? ' class="on"' : "") + ">" + esc(m.name) + "</a></li>";
    });
    tree.innerHTML = html + "</ul>";
    var on = tree.querySelector("a.on");
    if (on) on.scrollIntoView({ block: "nearest" });
  }

  var ALL = [];
  mods.forEach(function (m) {
    ALL.push({ m: m, d: null, t: m.name, k: "module" });
    m.decls.forEach(function (d) { ALL.push({ m: m, d: d, t: m.name === "core" ? d.name : m.name + "." + d.name, k: d.kind }); });
  });
  function find(q) {
    q = q.trim().toLowerCase();
    if (!q) { found.hidden = true; tree.hidden = false; return; }
    var hits = [];
    ALL.forEach(function (e) {
      var t = e.t.toLowerCase(), last = t.slice(t.lastIndexOf(".") + 1);
      var s = last === q ? 0 : last.indexOf(q) === 0 ? 1 : t.indexOf(q) === 0 ? 2 : t.indexOf(q) >= 0 ? 3 : -1;
      if (s >= 0) hits.push([s, e]);
    });
    hits.sort(function (a, b) { return a[0] - b[0] || a[1].t.length - b[1].t.length || (a[1].t < b[1].t ? -1 : 1); });
    found.innerHTML = hits.slice(0, 200).map(function (h) {
      var e = h[1];
      return '<li><a href="' + href(e.m, e.d) + '"><span>' + esc(e.t) + '</span><span class="kind">' + esc(e.k) + "</span></a></li>";
    }).join("") || '<li style="color:var(--muted);padding:4px 6px">Nothing matches.</li>';
    found.hidden = false;
    tree.hidden = true;
  }
  filter.addEventListener("input", function () { find(filter.value); });
  filter.addEventListener("keydown", function (e) {
    if (e.key === "Enter") { var a = found.querySelector("a"); if (a) location.hash = a.getAttribute("href"); }
    if (e.key === "Escape") { filter.value = ""; find(""); }
  });

  /* ---- right: the module's names ---- */
  function renderRight(m, cur) {
    if (!m) { right.innerHTML = ""; return; }
    var g = grouped(m), html = '<h4><a href="' + href(m) + '" style="padding:0;font-family:var(--sans);font-size:12px">' + esc(m.name) + "</a></h4>";
    GROUP_ORDER.forEach(function (k) {
      if (!g[k]) return;
      html += "<h4>" + k + "</h4><ul>" + g[k].map(function (d) {
        return '<li><a href="' + href(m, d) + '"' + (d === cur ? ' class="on"' : "") + ">" + esc(shortName(m, d)) + "</a></li>";
      }).join("") + "</ul>";
    });
    right.innerHTML = html;
    var on = right.querySelector("a.on");
    if (on) on.scrollIntoView({ block: "nearest" });
  }

  /* ---- middle ---- */
  function overview() {
    var html = "<h1 style=\"font-family:var(--sans)\">Standard library</h1>" +
      '<p class="lede">Every module of the Iris standard library. A program imports a module (<code>import str</code>) and uses its names with its name in front (<code>str.cut</code>). <code>core</code>, with <code>List</code> and <code>Map</code>, needs no import. Generated from the library\'s source, Iris ' + esc(data.version) + ".</p>" +
      '<div class="modgrid" style="margin-top:28px">';
    var group = null;
    mods.forEach(function (m) {
      if (m.group !== group) { if (group !== null) html += "</section>"; group = m.group; html += "<section><h4>" + esc(group) + "</h4>"; }
      html += '<a href="' + href(m) + '">' + esc(m.name) + "</a><span>" + esc(m.summary) + "</span>";
    });
    main.innerHTML = html + "</section></div>";
  }

  function members(m, list) {
    return '<div class="members">' + list.map(function (d) {
      return '<a href="' + href(m, d) + '">' + esc(shortName(m, d)) + "</a><span>" + inline(firstSentence(d.doc)) + "</span>";
    }).join("") + "</div>";
  }

  function modulePage(m) {
    var g = grouped(m);
    var html = '<div class="crumb"><a href="#">Library</a> / ' + esc(m.group) + '<span class="badge">module</span></div>' +
      "<h1>" + esc(m.name) + "</h1>" + '<p class="lede">' + inline(m.summary) + "</p>" +
      (m.name === "core" ? "" : '<pre class="code imp"><code>' + hl("import " + m.name) + "</code></pre>") +
      (m.intro ? '<div class="intro prose">' + m.intro + "</div>" : m.doc ? '<div class="intro prose">' + docHtml(m.doc) + "</div>" : "");
    GROUP_ORDER.forEach(function (k) {
      if (!g[k]) return;
      html += "<h2>" + k + " (" + g[k].length + ")</h2>";
      if (k === "Constants") {
        html += '<div class="table"><table><tbody>' + g[k].map(function (d) {
          return '<tr><td><a href="' + href(m, d) + '"><code>' + esc(shortName(m, d)) + "</code></a></td><td><code>" + esc(d.sig) + "</code></td><td>" + inline(firstSentence(d.doc)) + "</td></tr>";
        }).join("") + "</tbody></table></div>";
      } else {
        html += members(m, g[k]);
      }
    });
    main.innerHTML = html;
  }

  function declPage(m, d) {
    var short = shortName(m, d);
    var html = '<div class="crumb"><a href="#">Library</a> / <a href="' + href(m) + '">' + esc(m.name) + '</a><span class="badge">' + esc(d.kind) + "</span>" +
      (d.platform && d.platform.length ? '<span class="badge">' + esc(d.platform.join(", ")) + "</span>" : "") + "</div>" +
      "<h1>" + esc(short) + "</h1>" +
      '<pre class="code sig"><code>' + hl(d.sig) + "</code></pre>";
    html += d.doc ? '<div class="docline prose">' + docHtml(d.doc) + "</div>" : '<p class="docline" style="color:var(--muted)">No description yet.</p>';
    if (d.kind === "struct") {
      var type = d.name.split(".").pop();
      var methods = [];
      mods.forEach(function (mm) {
        mm.decls.forEach(function (x) {
          if (x.kind === "method" && x.name.split(".").slice(-2)[0] === type && (mm === m || m.name === "core")) methods.push([mm, x]);
        });
      });
      if (methods.length) {
        html += "<h2>Methods (" + methods.length + ")</h2>" + '<div class="members">' + methods.map(function (p) {
          return '<a href="' + href(p[0], p[1]) + '">' + esc(shortName(p[0], p[1])) + "</a><span>" + inline(firstSentence(p[1].doc)) + "</span>";
        }).join("") + "</div>";
      }
    }
    html += '<div class="from">Defined in <a href="https://github.com/ozzenerol/iris/blob/main/std/' + d.file + "#L" + d.line + '">std/' + esc(d.file) + ":" + d.line + "</a></div>";
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
    if (!m) { overview(); document.title = "Library - Iris"; }
    else if (!d) { modulePage(m); document.title = m.name + " - Iris"; }
    else { declPage(m, d); document.title = shortName(m, d) + " (" + m.name + ") - Iris"; }
    renderTree(m);
    renderRight(m, d);
    main.scrollTop = 0;
    window.scrollTo(0, 0);
  }

  window.addEventListener("hashchange", route);
  route();
})();

/* The search box, the theme switch, Copy buttons and "On this page". */
(function () {
  var box = document.querySelector(".search");
  var input = document.getElementById("site-search");
  var list = document.getElementById("site-results");
  var root = box ? box.getAttribute("data-root") : "";
  var entries = window.IRIS_SEARCH || [];
  var sel = -1;

  /* best first: exact, prefix of the name or its last part, then contains */
  function score(e, q) {
    var t = e.t.toLowerCase();
    var last = t.slice(t.lastIndexOf(".") + 1);
    if (t === q || last === q) return 0;
    if (last.indexOf(q) === 0) return 1;
    if (t.indexOf(q) === 0) return 2;
    var i = t.indexOf(q);
    if (i >= 0) return 3 + i / 100;
    if ((e.p || "").toLowerCase().indexOf(q) >= 0) return 6;
    return -1;
  }

  function show(q) {
    q = q.trim().toLowerCase();
    sel = -1;
    if (!q) { list.hidden = true; list.innerHTML = ""; return; }
    var hits = [];
    for (var i = 0; i < entries.length; i++) {
      var s = score(entries[i], q);
      if (s >= 0) hits.push([s, entries[i]]);
    }
    hits.sort(function (a, b) { return a[0] - b[0] || a[1].t.length - b[1].t.length; });
    hits = hits.slice(0, 30);
    if (!hits.length) {
      list.innerHTML = '<li class="empty">Nothing matches "' + q.replace(/[<&]/g, "") + '".</li>';
    } else {
      list.innerHTML = hits.map(function (h) {
        var e = h[1];
        return '<li><a href="' + root + e.u + '"><span class="r-t">' + esc(e.t) +
          '</span><span class="r-p">' + esc(e.k === "section" || e.k === "chapter" ? e.p : e.k + " · " + e.p) + "</span></a></li>";
      }).join("");
    }
    list.hidden = false;
  }

  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;"); }

  function move(d) {
    var links = list.querySelectorAll("a");
    if (!links.length) return;
    sel = (sel + d + links.length) % links.length;
    links.forEach(function (a, i) { a.classList.toggle("sel", i === sel); });
    links[sel].scrollIntoView({ block: "nearest" });
  }

  if (input) {
    input.addEventListener("input", function () { show(input.value); });
    input.addEventListener("keydown", function (e) {
      if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); move(-1); }
      else if (e.key === "Enter") {
        var links = list.querySelectorAll("a");
        var a = links[sel >= 0 ? sel : 0];
        if (a) { location.href = a.href; list.hidden = true; }
      } else if (e.key === "Escape") { input.value = ""; show(""); input.blur(); }
    });
    document.addEventListener("click", function (e) { if (!box.contains(e.target)) list.hidden = true; });
    document.addEventListener("keydown", function (e) {
      var t = e.target.tagName;
      if (e.key === "/" && t !== "INPUT" && t !== "TEXTAREA") { e.preventDefault(); input.focus(); }
    });
  }

  /* theme: the system's, until the button picks one (remembered) */
  var themeBtn = document.getElementById("theme");
  if (themeBtn) {
    themeBtn.addEventListener("click", function () {
      var dark = document.documentElement.dataset.theme
        ? document.documentElement.dataset.theme === "dark"
        : window.matchMedia("(prefers-color-scheme: dark)").matches;
      var next = dark ? "light" : "dark";
      document.documentElement.dataset.theme = next;
      try { localStorage.setItem("iris-theme", next); } catch (e) {}
    });
  }

  /* A Copy button copies el's text, or selects it when the clipboard is out of reach */
  function copyOnClick(b, el) {
    b.addEventListener("click", function () {
      var text = el ? el.textContent : "";
      function done() { b.textContent = "Copied"; setTimeout(function () { b.textContent = "Copy"; }, 1500); }
      function select() { var r = document.createRange(); r.selectNodeContents(el); var s = getSelection(); s.removeAllRanges(); s.addRange(r); }
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, select);
      else select();
    });
  }
  document.querySelectorAll("button.copy").forEach(function (b) {
    copyOnClick(b, document.getElementById(b.getAttribute("data-copy")));
  });
  /* and every code block of a page has one */
  document.querySelectorAll(".prose pre.code:not(.bad)").forEach(function (pre) {
    var b = document.createElement("button");
    b.className = "copy copy-code";
    b.type = "button";
    b.textContent = "Copy";
    pre.appendChild(b);
    copyOnClick(b, pre.querySelector("code"));
  });

  /* closable sidebars: chapters, on this page, and the library's two panes.
     the open/closed class lives on <html> (set early, in <head>, to avoid a layout
     flash) and the CSS keys off it; this just keeps buttons and storage in sync. */
  var SIDES = { chapters: "left", onpage: "right", "std-left": "left", "std-right": "right" };
  function syncButtons(side, closed) {
    var left = SIDES[side] === "left";
    var open = left ? "‹" : "›";
    var shut = left ? "›" : "‹";
    document.querySelectorAll('.side-toggle[data-side="' + side + '"]').forEach(function (b) {
      var icon = closed ? shut : open;
      var text = b.getAttribute("data-text");
      b.textContent = left ? icon + " " + text : text + " " + icon;
      b.setAttribute("aria-expanded", String(!closed));
      b.setAttribute("aria-label", (closed ? "Show " : "Hide ") + b.getAttribute("data-label"));
    });
  }
  Object.keys(SIDES).forEach(function (side) {
    var closed = document.documentElement.classList.contains("closed-" + side);
    syncButtons(side, closed);
  });
  document.querySelectorAll(".side-toggle").forEach(function (b) {
    b.addEventListener("click", function () {
      var side = b.getAttribute("data-side");
      var cls = "closed-" + side;
      var closed = !document.documentElement.classList.contains(cls);
      document.documentElement.classList.toggle(cls, closed);
      syncButtons(side, closed);
      try { localStorage.setItem("iris-side-" + side, closed ? "closed" : "open"); } catch (e) {}
    });
  });

  /* "On this page": the section being read */
  var links = document.querySelectorAll(".onpage a");
  if (links.length && "IntersectionObserver" in window) {
    var byId = {};
    links.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
    var seen = {};
    var obs = new IntersectionObserver(function (es) {
      es.forEach(function (e) { seen[e.target.id] = e.isIntersecting; });
      var first = null;
      document.querySelectorAll(".prose h2[id]").forEach(function (h) { if (!first && seen[h.id]) first = h.id; });
      if (first) links.forEach(function (a) { a.classList.toggle("on", a === byId[first]); });
    }, { rootMargin: "-60px 0px -60% 0px" });
    document.querySelectorAll(".prose h2[id]").forEach(function (h) { obs.observe(h); });
  }
})();

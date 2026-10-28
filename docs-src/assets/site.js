/* The search bar, the theme switch, Copy buttons, the sidebar drawer on
   small screens, the heading being read, the library's panes and the arrow keys. */
(function () {
  var html = document.documentElement;
  document.querySelectorAll(".ver").forEach(function (v) {
    v.textContent = window.IRIS_VERSION || "";
  });

  /* ---- search: the bar over the text, its results under it ---- */
  var bar = document.querySelector(".toolbar");
  var input = document.getElementById("site-search");
  var list = document.getElementById("site-results");
  var root = bar ? bar.getAttribute("data-root") : "";
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
  function esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;"); }

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
    hits = hits.slice(0, 40);
    list.hidden = false;
    if (!hits.length) {
      list.innerHTML = '<li class="empty">Nothing matches "' + esc(q) + '".</li>';
      return;
    }
    list.innerHTML = hits.map(function (h) {
      var e = h[1];
      var where = e.k === "section" || e.k === "chapter" ? e.p : e.k + " in " + e.p;
      return '<li><a href="' + root + e.u + '"><span class="r-t">' + esc(e.t) +
        '</span><span class="r-p">' + esc(where) + "</span></a></li>";
    }).join("");
  }
  function move(d) {
    var links = list.querySelectorAll("a");
    if (!links.length) return;
    sel = (sel + d + links.length) % links.length;
    links.forEach(function (a, i) { a.classList.toggle("sel", i === sel); });
    links[sel].scrollIntoView({ block: "nearest" });
  }
  function closeSearch() {
    if (!input) return;
    input.value = "";
    show("");
    input.blur();
  }
  if (input) {
    input.addEventListener("input", function () { show(input.value); });
    input.addEventListener("focus", function () { if (input.value) show(input.value); });
    input.addEventListener("keydown", function (e) {
      if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); move(-1); }
      else if (e.key === "Enter") {
        var a = list.querySelectorAll("a")[sel >= 0 ? sel : 0];
        if (a) { list.hidden = true; location.href = withTheme(a.href); }
      }
    });
    document.addEventListener("click", function (e) { if (!e.target.closest(".search-form")) list.hidden = true; });
  }

  /* keys: / or Ctrl+K searches, Esc closes, the arrows go to the page before and after */
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") { closeSearch(); setNav(false); return; }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k" && input) { e.preventDefault(); input.focus(); return; }
    var t = e.target.tagName;
    if (t === "INPUT" || t === "TEXTAREA" || e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
    if ((e.key === "/" || e.key === "s") && input) { e.preventDefault(); input.focus(); return; }
    var rel = e.key === "ArrowLeft" ? "prev" : e.key === "ArrowRight" ? "next" : "";
    var a = rel && document.querySelector('.pager a[rel="' + rel + '"]');
    if (a) { e.preventDefault(); location.href = withTheme(a.href); }
  });

  /* ---- theme: the system's, until the button picks one (remembered) ---- */
  var themeBtn = document.getElementById("theme");
  if (themeBtn) {
    themeBtn.addEventListener("click", function () {
      var dark = html.dataset.theme
        ? html.dataset.theme === "dark"
        : window.matchMedia("(prefers-color-scheme: dark)").matches;
      var next = dark ? "light" : "dark";
      html.dataset.theme = next;
      try { localStorage.setItem("iris-theme", next); } catch (e) {}
      window.name = "iris-theme:" + next;
      history.replaceState(history.state, "", withTheme(location.href));
    });
  }
  /* Back and Forward can show a page as it was left, without running this
     again: it takes the theme picked since then */
  window.addEventListener("pageshow", function () {
    var m = /^iris-theme:(dark|light)$/.exec(window.name), t = m && m[1];
    try { t = localStorage.getItem("iris-theme") || t; } catch (e) {}
    if (t && html.dataset.theme !== t) {
      html.dataset.theme = t;
      history.replaceState(history.state, "", withTheme(location.href));
    }
  });
  /* Opened as files, each page has storage of its own in some browsers, so
     a link to another page of the site carries the theme in its address. */
  function withTheme(url) {
    var t = html.dataset.theme;
    var u = new URL(url, location.href);
    if (!t || u.protocol !== location.protocol || u.host !== location.host) return url;
    u.searchParams.set("theme", t);
    return u.href;
  }
  function carryTheme(e) {
    var a = e.target.closest && e.target.closest("a[href]");
    if (a && a.getAttribute("href").charAt(0) !== "#") a.href = withTheme(a.href);
  }
  document.addEventListener("click", carryTheme, true);
  document.addEventListener("auxclick", carryTheme, true);
  document.addEventListener("contextmenu", carryTheme, true);

  /* ---- Copy buttons ---- */
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

  /* ---- the chapter list: a drawer on small screens ---- */
  var navBtn = document.getElementById("nav-toggle");
  var scrim = document.getElementById("scrim");
  function setNav(open) {
    if (!navBtn) return;
    html.classList.toggle("nav-open", open);
    navBtn.setAttribute("aria-expanded", String(open));
    if (scrim) scrim.hidden = !open;
  }
  if (navBtn) {
    navBtn.addEventListener("click", function () { setNav(!html.classList.contains("nav-open")); });
    if (scrim) scrim.addEventListener("click", function () { setNav(false); });
  }
  /* the page being read is in view in the chapter list */
  var current = document.querySelector(".sidebar .block a.current");
  if (current) {
    var scroller = current.closest(".sidebar");
    var top = current.getBoundingClientRect().top - scroller.getBoundingClientRect().top;
    if (top > scroller.clientHeight - 80) scroller.scrollTop = top - scroller.clientHeight / 3;
  }

  /* ---- the heading being read, under its chapter in the sidebar ---- */
  var tocLinks = document.querySelectorAll(".sidebar .headings a");
  if (tocLinks.length && "IntersectionObserver" in window) {
    var byId = {};
    tocLinks.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
    var heads = Array.prototype.filter.call(document.querySelectorAll(".prose h2[id]"), function (h) { return byId[h.id]; });
    var seen = {};
    var obs = new IntersectionObserver(function (es) {
      es.forEach(function (e) { seen[e.target.id] = e.isIntersecting; });
      var first = null;
      heads.forEach(function (h) { if (!first && seen[h.id]) first = h.id; });
      if (first) tocLinks.forEach(function (a) { a.classList.toggle("on", a === byId[first]); });
    }, { rootMargin: "0px 0px -55% 0px" });
    heads.forEach(function (h) { obs.observe(h); });
  }

  /* ---- the library's two panes: the open/closed class lives on <html> (set
     early, in <head>, to avoid a layout flash); this keeps aria-expanded and
     storage in sync ---- */
  function sync(side) {
    var open = !html.classList.contains("closed-" + side);
    document.querySelectorAll('.side-btn[data-side="' + side + '"]').forEach(function (b) {
      b.setAttribute("aria-expanded", String(open));
    });
  }
  ["std-left", "std-right"].forEach(sync);
  document.querySelectorAll(".side-btn").forEach(function (b) {
    b.addEventListener("click", function () {
      var side = b.getAttribute("data-side");
      var closed = html.classList.toggle("closed-" + side);
      sync(side);
      try { localStorage.setItem("iris-side-" + side, closed ? "closed" : "open"); } catch (e) {}
    });
  });
})();

/* rustdoc's ways: the search (its results take the place of the page), the
   help and settings popovers, the themes, Copy buttons, the sidebar on
   phones, the heading being read and the keys. */
(function () {
  var html = document.documentElement;
  document.querySelectorAll(".ver").forEach(function (v) {
    v.textContent = window.IRIS_VERSION || "";
  });

  /* ---- search: the results take the place of the page until Esc ---- */
  var bar = document.querySelector("nav.sub");
  var input = document.getElementById("site-search");
  var mainContent = document.getElementById("main-content");
  var results = document.getElementById("search");
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

  /* a result's name: its module (or chapter) as the path, its last part in its kind's colour */
  function nameHtml(e) {
    var dot = e.k !== "section" && e.k !== "chapter" && e.k !== "page" ? e.t.lastIndexOf(".") : -1;
    var path = dot >= 0 ? '<span class="path">' + esc(e.t.slice(0, dot + 1)) + "</span>" : "";
    return path + '<span class="kind-' + esc(e.k) + '">' + esc(dot >= 0 ? e.t.slice(dot + 1) : e.t) + "</span>";
  }

  function show(q) {
    var raw = q.trim();
    q = raw.toLowerCase();
    sel = -1;
    if (!results || !mainContent) return;
    if (!q) { results.hidden = true; results.innerHTML = ""; mainContent.hidden = false; return; }
    var hits = [];
    for (var i = 0; i < entries.length; i++) {
      var s = score(entries[i], q);
      if (s >= 0) hits.push([s, entries[i]]);
    }
    hits.sort(function (a, b) { return a[0] - b[0] || a[1].t.length - b[1].t.length; });
    var shown = hits.slice(0, 200);
    var list = shown.map(function (h) {
      var e = h[1];
      var where = e.k === "section" || e.k === "chapter" ? e.p : e.k + " in " + e.p;
      return '<li><a href="' + root + e.u + '"><span class="result-name">' + nameHtml(e) +
        '</span><span class="desc">' + esc(where) + "</span></a></li>";
    }).join("");
    results.innerHTML = '<div class="main-heading search-results-title"><h1>Results for ' + esc(raw) + "</h1></div>" +
      '<div class="tabs"><button type="button">In Names <span class="count">(' + hits.length + ")</span></button></div>" +
      (hits.length ? '<ul class="search-results">' + list + "</ul>"
        : '<div class="search-failed">No results :(<br>Try a shorter word, or a name from the library such as <code>str.cut</code>.</div>');
    results.hidden = false;
    mainContent.hidden = true;
  }
  function move(d) {
    var links = results ? results.querySelectorAll(".search-results a") : [];
    if (!links.length) return;
    sel = (sel + d + links.length) % links.length;
    links.forEach(function (a, i) { a.classList.toggle("sel", i === sel); });
    links[sel].scrollIntoView({ block: "nearest" });
    links[sel].focus({ preventScroll: true });
  }
  function closeSearch() {
    if (!input) return;
    input.value = "";
    show("");
    input.blur();
  }
  if (input) {
    input.addEventListener("input", function () { show(input.value); });
    input.addEventListener("keydown", function (e) {
      if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
      else if (e.key === "Enter") {
        var a = results.querySelectorAll(".search-results a")[sel >= 0 ? sel : 0];
        if (a) location.href = withTheme(a.href);
      }
    });
  }
  if (results) {
    results.addEventListener("keydown", function (e) {
      if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); if (sel <= 0) { sel = -1; input.focus(); } else move(-1); }
    });
  }

  /* ---- the help (?) and settings popovers ---- */
  function popover(id, btn) {
    var box = document.getElementById(id);
    if (!btn || !box) return null;
    var a = btn.querySelector("a");
    a.addEventListener("click", function (e) {
      e.preventDefault();
      var open = box.hidden;
      closePopovers();
      box.hidden = !open;
    });
    return box;
  }
  var pops = [popover("help", document.getElementById("help-button")),
              popover("settings", document.getElementById("settings-menu"))].filter(Boolean);
  function closePopovers() { pops.forEach(function (p) { p.hidden = true; }); }
  document.addEventListener("click", function (e) {
    if (!e.target.closest("#help-button, #settings-menu")) closePopovers();
  });

  /* ---- keys: S or / searches, ? helps, Esc closes, the arrows go to the page before and after ---- */
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") { closePopovers(); closeSearch(); setNav(false); return; }
    var t = e.target.tagName;
    if (t === "INPUT" || t === "TEXTAREA" || e.altKey || e.ctrlKey || e.metaKey) return;
    if ((e.key === "/" || e.key === "s" || e.key === "S") && input) { e.preventDefault(); input.focus(); return; }
    if (e.key === "?") {
      e.preventDefault();
      var help = document.getElementById("help");
      if (help) { var open = help.hidden; closePopovers(); help.hidden = !open; }
      return;
    }
    if (e.shiftKey) return;
    var rel = e.key === "ArrowLeft" ? "prev" : e.key === "ArrowRight" ? "next" : "";
    var a = rel && document.querySelector('.pager a[rel="' + rel + '"]');
    if (a) { e.preventDefault(); location.href = withTheme(a.href); }
  });

  /* ---- themes: the system's (light or dark) until one is picked, then that one, remembered ---- */
  function setTheme(t) {
    if (t === "system") delete html.dataset.theme;
    else html.dataset.theme = t;
    try { if (t === "system") localStorage.removeItem("iris-theme"); else localStorage.setItem("iris-theme", t); } catch (e) {}
    window.name = t === "system" ? "" : "iris-theme:" + t;
    var u = new URL(location.href);
    u.searchParams.delete("theme");
    history.replaceState(history.state, "", t === "system" ? u.href : withTheme(u.href));
  }
  function syncRadios() {
    var cur = html.dataset.theme || "system";
    document.querySelectorAll('#settings input[name="theme"]').forEach(function (r) { r.checked = r.value === cur; });
  }
  document.querySelectorAll('#settings input[name="theme"]').forEach(function (r) {
    r.addEventListener("change", function () { if (r.checked) setTheme(r.value); });
  });
  syncRadios();
  /* Back and Forward can show a page as it was left, without running this
     again: it takes the theme picked since then */
  window.addEventListener("pageshow", function () {
    var m = /^iris-theme:(dark|light|ayu)$/.exec(window.name), t = m && m[1];
    try { t = localStorage.getItem("iris-theme") || t; } catch (e) {}
    if (t && html.dataset.theme !== t) html.dataset.theme = t;
    syncRadios();
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

  /* ---- Copy buttons on code blocks ---- */
  function copyOnClick(b, el) {
    b.addEventListener("click", function () {
      var text = el ? el.textContent : "";
      function done() { b.textContent = "Copied"; setTimeout(function () { b.textContent = "Copy"; }, 1500); }
      function select() { var r = document.createRange(); r.selectNodeContents(el); var s = getSelection(); s.removeAllRanges(); s.addRange(r); }
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, select);
      else select();
    });
  }
  window.irisCopyButtons = function (scope) {
    scope.querySelectorAll(".docblock pre.code:not(.bad)").forEach(function (pre) {
      if (pre.querySelector(".copy-code")) return;
      var b = document.createElement("button");
      b.className = "copy-code";
      b.type = "button";
      b.textContent = "Copy";
      pre.appendChild(b);
      copyOnClick(b, pre.querySelector("code"));
    });
  };
  window.irisCopyButtons(document);

  /* ---- the sidebar: slides over the page on phones ---- */
  var navBtn = document.getElementById("nav-toggle");
  var scrim = document.getElementById("scrim");
  function setNav(open) {
    if (!navBtn) return;
    html.classList.toggle("nav-open", open);
    navBtn.setAttribute("aria-expanded", String(open));
    if (scrim) scrim.hidden = !open;
  }
  window.irisCloseNav = function () { setNav(false); };
  if (navBtn) {
    navBtn.addEventListener("click", function () { setNav(!html.classList.contains("nav-open")); });
    if (scrim) scrim.addEventListener("click", function () { setNav(false); });
  }
  /* the page being read is in view in the sidebar */
  var current = document.querySelector(".sidebar .block:not(.crate) a.current");
  if (current) {
    var scroller = current.closest(".sidebar");
    var top = current.getBoundingClientRect().top - scroller.getBoundingClientRect().top;
    if (top > scroller.clientHeight - 80) scroller.scrollTop = top - scroller.clientHeight / 3;
  }

  /* ---- the heading being read, under Sections in the sidebar ---- */
  var tocLinks = document.querySelectorAll(".sidebar .top-toc a");
  if (tocLinks.length && "IntersectionObserver" in window) {
    var byId = {};
    tocLinks.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
    var heads = Array.prototype.filter.call(document.querySelectorAll(".docblock h2[id]"), function (h) { return byId[h.id]; });
    var seen = {};
    var obs = new IntersectionObserver(function (es) {
      es.forEach(function (e) { seen[e.target.id] = e.isIntersecting; });
      var first = null;
      heads.forEach(function (h) { if (!first && seen[h.id]) first = h.id; });
      if (first) tocLinks.forEach(function (a) { a.classList.toggle("on", a === byId[first]); });
    }, { rootMargin: "0px 0px -55% 0px" });
    heads.forEach(function (h) { obs.observe(h); });
  }
})();

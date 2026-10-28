/* The sidebar, the search, the theme switch, Copy buttons, the library's panes and the arrow keys. */
(function () {
  document.querySelectorAll(".ver").forEach(function (v) {
    v.textContent = window.IRIS_VERSION || "";
  });
  var html = document.documentElement;
  var box = document.getElementById("search-wrapper");
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

  var searchBtn = document.getElementById("search-toggle");
  function openSearch(open) {
    if (!box) return;
    box.hidden = !open;
    searchBtn.setAttribute("aria-expanded", String(open));
    if (open) { input.focus(); input.select(); }
  }
  if (searchBtn) searchBtn.addEventListener("click", function () { openSearch(box.hidden); });

  if (input) {
    input.addEventListener("input", function () { show(input.value); });
    input.addEventListener("keydown", function (e) {
      if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); move(-1); }
      else if (e.key === "Enter") {
        var links = list.querySelectorAll("a");
        var a = links[sel >= 0 ? sel : 0];
        if (a) { location.href = withTheme(a.href); list.hidden = true; }
      } else if (e.key === "Escape") { input.value = ""; show(""); openSearch(false); searchBtn.focus(); }
    });
  }

  /* keys: / or s searches, the arrows go to the page before and after */
  document.addEventListener("keydown", function (e) {
    var t = e.target.tagName;
    if (t === "INPUT" || t === "TEXTAREA" || e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
    if ((e.key === "/" || e.key === "s") && input) { e.preventDefault(); openSearch(true); return; }
    var rel = e.key === "ArrowLeft" ? "prev" : e.key === "ArrowRight" ? "next" : "";
    var a = rel && document.querySelector('.pager a[rel="' + rel + '"]');
    if (a) { e.preventDefault(); location.href = withTheme(a.href); }
  });

  /* the sidebar: the button opens and closes it; on a wide screen that is
     remembered, on a narrow one it opens over the text and a click on the
     text or a link closes it */
  var sidebar = document.getElementById("sidebar");
  var sideBtn = document.getElementById("sidebar-toggle");
  function narrow() { return window.innerWidth < 1080; }
  function setSidebar(visible, remember) {
    html.classList.toggle("sidebar-visible", visible);
    html.classList.toggle("sidebar-hidden", !visible);
    if (sideBtn) sideBtn.setAttribute("aria-expanded", String(visible));
    if (remember) { try { localStorage.setItem("iris-sidebar", visible ? "visible" : "hidden"); } catch (e) {} }
  }
  if (sideBtn) {
    sideBtn.setAttribute("aria-expanded", String(html.classList.contains("sidebar-visible")));
    sideBtn.addEventListener("click", function () {
      setSidebar(!html.classList.contains("sidebar-visible"), !narrow());
    });
    document.addEventListener("click", function (e) {
      if (narrow() && html.classList.contains("sidebar-visible") && !sidebar.contains(e.target) && !sideBtn.contains(e.target)) {
        setSidebar(false, false);
      }
    });
  }
  /* the page being read is in view in the sidebar */
  var current = sidebar && sidebar.querySelector("a.active");
  if (current) {
    var scroller = sidebar.querySelector(".sidebar-scroll");
    var top = current.getBoundingClientRect().top - scroller.getBoundingClientRect().top;
    if (top > scroller.clientHeight - 80) scroller.scrollTop = top - scroller.clientHeight / 3;
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
      window.name = "iris-theme:" + next;
      history.replaceState(history.state, "", withTheme(location.href));
    });
  }
  /* Back and Forward can show a page as it was left, without running this
     again: it takes the theme picked since then */
  window.addEventListener("pageshow", function () {
    var m = /^iris-theme:(dark|light)$/.exec(window.name), t = m && m[1];
    try { t = localStorage.getItem("iris-theme") || t; } catch (e) {}
    if (t && document.documentElement.dataset.theme !== t) {
      document.documentElement.dataset.theme = t;
      history.replaceState(history.state, "", withTheme(location.href));
    }
  });
  /* Opened as files, each page has storage of its own in some browsers, so
     a link to another page of the site carries the theme in its address. */
  function withTheme(url) {
    var t = document.documentElement.dataset.theme;
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

  /* the library's two panes: the open/closed class lives on <html> (set early,
     in <head>, to avoid a layout flash); this keeps aria-expanded and storage in sync */
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

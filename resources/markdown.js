// Gira nel "mondo" isolato dell'applicazione: il JavaScript del documento è disattivato,
// ma questo script vede lo stesso DOM. Va eseguito dopo highlight.js e KaTeX.
(() => {
    "use strict";

    // ── Ancore sui titoli, con gli stessi slug di GitHub ──────────────────
    const slug = (text) => text.trim().toLowerCase()
        .replace(/[^\p{L}\p{N}\s_-]/gu, "")
        .replace(/\s/g, "-");

    const used = new Map();
    for (const h of document.querySelectorAll("article h1, article h2, article h3, article h4, article h5, article h6")) {
        let id = slug(h.textContent) || "sezione";
        const n = used.get(id) || 0;
        used.set(id, n + 1);
        if (n) id += "-" + n;
        h.id = id;
        const a = document.createElement("a");
        a.className = "anchor";
        a.href = "#" + id;
        a.textContent = "#";
        h.prepend(a);
    }

    // Titoli per il pannello indice. `top` in pixel CSS dall'inizio del documento:
    // va richiesto di nuovo quando immagini e formule cambiano l'altezza della pagina.
    window.__disOutline = () => [...document.querySelectorAll("article :is(h1, h2, h3, h4, h5, h6)")].map((h) => ({
        level: Number(h.tagName[1]),
        id: h.id,
        text: [...h.childNodes].filter((n) => !(n.classList && n.classList.contains("anchor")))
                               .map((n) => n.textContent).join("").trim(),
        top: h.getBoundingClientRect().top + window.scrollY,
    }));

    // Salto a una sezione: accetta un id esatto o il testo del titolo (wikilink [[Nota#Titolo]]).
    window.__disGoto = (fragment) => {
        let target = decodeURIComponent(fragment);
        const el = document.getElementById(target) || document.getElementById(slug(target));
        if (el) el.scrollIntoView({ block: "start" });
        return !!el;
    };

    // md4c trasforma "> [!NOTE]" senza titolo in div.admonition-note: stessa resa.
    for (const div of document.querySelectorAll("article div[class^='admonition-']")) {
        const type = div.className.slice("admonition-".length).toLowerCase();
        const head = div.querySelector(":scope > p.admonition-title");
        const title = head ? head.innerHTML.trim() : "";
        if (head) head.remove();
        const bq = document.createElement("blockquote");
        const p = document.createElement("p");
        p.textContent = "[!" + type + "] ";
        p.insertAdjacentHTML("beforeend", title === type ? "" : title);
        bq.append(p, ...div.childNodes);
        div.replaceWith(bq);
    }

    // ── Callout di Obsidian e GitHub: > [!tipo] Titolo ─────────────────────
    const icons = {
        info:    '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/>',
        tip:     '<path d="M9 18h6M10 22h4M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z"/>',
        check:   '<path d="M20 6 9 17l-5-5"/>',
        alert:   '<path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0zM12 9v4M12 17h.01"/>',
        danger:  '<circle cx="12" cy="12" r="10"/><path d="m15 9-6 6M9 9l6 6"/>',
        quote:   '<path d="M3 21c3 0 7-1 7-8V5H3v8h4M14 21c3 0 7-1 7-8V5h-7v8h4"/>',
        list:    '<path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/>',
        help:    '<circle cx="12" cy="12" r="10"/><path d="M9.1 9a3 3 0 0 1 5.8 1c0 2-3 3-3 3M12 17h.01"/>',
    };
    const iconFor = {
        note: "info", info: "info", tip: "tip", hint: "tip", important: "tip",
        success: "check", check: "check", done: "check", todo: "check",
        warning: "alert", caution: "alert", attention: "alert",
        danger: "danger", error: "danger", bug: "danger", failure: "danger", fail: "danger", missing: "danger",
        quote: "quote", cite: "quote", abstract: "list", summary: "list", tldr: "list", example: "list",
        question: "help", faq: "help", help: "help",
    };

    for (const bq of document.querySelectorAll("article blockquote")) {
        const first = bq.firstElementChild;
        if (!first || first.tagName !== "P") continue;
        const m = first.innerHTML.match(/^\s*\[!(\w+)\][+-]?[ \t]*([^\n]*)\n?/);
        if (!m) continue;

        const type = m[1].toLowerCase();
        const title = m[2].trim() || type.charAt(0).toUpperCase() + type.slice(1);
        first.innerHTML = first.innerHTML.slice(m[0].length);
        if (!first.textContent.trim() && !first.querySelector("img")) first.remove();

        const box = document.createElement("div");
        box.className = "callout";
        box.dataset.type = type;
        box.innerHTML =
            '<div class="callout-title"><svg class="callout-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
            'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' + icons[iconFor[type] || "info"] +
            "</svg><span></span></div>";
        box.querySelector(".callout-title span").innerHTML = title;
        box.append(...bq.childNodes);
        bq.replaceWith(box);
    }

    // ── Formule ───────────────────────────────────────────────────────────
    const renderMath = (el, tex, display) => {
        try {
            katex.render(tex, el, { displayMode: display, throwOnError: false, output: "htmlAndMathml" });
        } catch (e) {
            el.textContent = tex;
            el.classList.add("katex-error");
        }
    };
    if (typeof katex !== "undefined") {
        for (const eq of document.querySelectorAll("x-equation"))
            renderMath(eq, eq.textContent, eq.getAttribute("type") === "display");
    }

    // ── Blocchi di codice ─────────────────────────────────────────────────
    if (typeof hljs !== "undefined") hljs.configure({ ignoreUnescapedHTML: true });
    for (const code of document.querySelectorAll("pre > code")) {
        const pre = code.parentElement;
        const m = code.className.match(/language-([\w+#-]+)/);
        if (!m) continue;
        const lang = m[1].toLowerCase();

        // ```math come in GitHub: una formula in display.
        if ((lang === "math" || lang === "latex-math") && typeof katex !== "undefined") {
            const div = document.createElement("x-equation");
            div.setAttribute("type", "display");
            renderMath(div, code.textContent, true);
            pre.replaceWith(div);
            continue;
        }

        pre.dataset.lang = lang;
        if (typeof hljs !== "undefined" && hljs.getLanguage(lang))
            hljs.highlightElement(code);
    }
})();

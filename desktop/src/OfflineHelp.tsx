/// <reference types="vite/client" />
import React, { useState } from "react";
import { openUrl } from "./transport/dialogs";

const manuals = import.meta.glob<string>("../../docs/manual/*.md", { eager: true, query: "?raw", import: "default" });
const notes = import.meta.glob<string>("../../docs/release-notes/v*.md", { eager: true, query: "?raw", import: "default" });
const images = import.meta.glob<string>("../../docs/manual/images/*", { eager: true, query: "?url", import: "default" });
const online = "https://kpt1020.github.io/mib-studio-qt/";

function inline(text: string, navigate: (page: string) => void): React.ReactNode[] {
  return text.split(/(!?\[[^\]]*\]\([^)]*\)|\*\*[^*]+\*\*|`[^`]+`)/g).map((part, i) => {
    const link = /^(!?)\[([^\]]*)\]\(([^)]*)\)$/.exec(part);
    if (link) {
      const [, image, label, href] = link;
      if (image) return <img key={i} alt={label} src={images[`../../docs/manual/${href}`]} style={{ maxWidth: "100%" }} />;
      return <a key={i} href={href} onClick={e => {
        e.preventDefault();
        if (/^https?:\/\//.test(href)) void openUrl(href);
        else navigate(href);
      }}>{label}</a>;
    }
    if (part.startsWith("**")) return <strong key={i}>{part.slice(2, -2)}</strong>;
    if (part.startsWith("`")) return <code key={i}>{part.slice(1, -1)}</code>;
    return part;
  });
}

export function Markdown({ text, navigate }: { text: string; navigate: (page: string) => void }) {
  // Render the Markdown constructs used by the bundled operator guide without
  // executing HTML from release notes.
  return <>{text.split(/(```[\s\S]*?```|\n\s*\n)/).filter(block => block.trim()).map((block, i) => {
    if (block.startsWith("```")) return <pre key={i}><code>{block.replace(/^```[^\n]*\n/, "").replace(/```$/, "")}</code></pre>;
    if (/^\s*---\s*$/.test(block)) return <hr key={i} />;
    const heading = /^(#{1,6})\s+(.+)$/.exec(block);
    if (heading) return React.createElement(`h${heading[1].length}`, { key: i, id: heading[2].toLowerCase().replace(/[^a-z0-9]+/g, "-") }, inline(heading[2], navigate));
    const rows = block.trim().split("\n");
    if (rows.length >= 2 && /^\|?\s*:?-+/.test(rows[1])) {
      const cells = (row: string) => row.replace(/^\||\|$/g, "").split("|").map(cell => cell.trim());
      return <table key={i}><thead><tr>{cells(rows[0]).map((cell, j) => <th key={j}>{inline(cell, navigate)}</th>)}</tr></thead>
        <tbody>{rows.slice(2).map((row, j) => <tr key={j}>{cells(row).map((cell, k) => <td key={k}>{inline(cell, navigate)}</td>)}</tr>)}</tbody></table>;
    }
    if (/^[-*] /.test(block)) return <ul key={i}>{block.split(/\n(?=[-*] )/).map((line, j) => <li key={j}>{inline(line.replace(/^[-*] /, ""), navigate)}</li>)}</ul>;
    if (/^\d+\. /.test(block)) return <ol key={i}>{block.split(/\n(?=\d+\. )/).map((line, j) => <li key={j}>{inline(line.replace(/^\d+\. /, ""), navigate)}</li>)}</ol>;
    if (block.startsWith("> ")) return <blockquote key={i}>{inline(block.replace(/^> ?/gm, ""), navigate)}</blockquote>;
    return <p key={i} style={{ whiteSpace: "pre-wrap" }}>{inline(block, navigate)}</p>;
  })}</>;
}

export function releaseNotesForVersion(content: Record<string, string>, version: string): string {
  const numeric = (v: string) => v.replace(/^.*\/v/, "").split(/[.-]/).slice(0, 3).map(Number);
  const compare = (a: string, b: string) => {
    const x = numeric(a), y = numeric(b);
    return x[0] - y[0] || x[1] - y[1] || x[2] - y[2];
  };
  const current = `v${version}.md`;
  return Object.keys(content).filter(path => path.endsWith(`/${current}`) || compare(path, version) < 0)
    .sort((a, b) => a.endsWith(`/${current}`) ? -1 : b.endsWith(`/${current}`) ? 1 : compare(b, a))
    .map(path => content[path]).join("\n\n---\n\n") || "# What's New\n\nNo release notes are bundled for this version.";
}

export function OfflineHelp({ kind, version, close }: { kind: "notes" | "manual"; version: string; close: () => void }) {
  const [page, setPage] = useState("index.md");
  const title = kind === "manual" ? "User Manual" : "What's New";
  const navigate = (href: string) => {
    const [target, anchor] = href.split("#");
    if (target && manuals[`../../docs/manual/${target}`]) setPage(target);
    if (anchor) requestAnimationFrame(() => document.getElementById(anchor)?.scrollIntoView());
  };
  const text = kind === "manual" ? manuals[`../../docs/manual/${page}`] : releaseNotesForVersion(notes, version);
  return <div className="modal-backdrop" onClick={close}>
    <div className="modal" role="dialog" aria-label={title} onClick={e => e.stopPropagation()} style={{ width: "min(900px, 90vw)", maxHeight: "90vh", overflowY: "auto" }}>
      <h2>{title}</h2>
      {kind === "manual" && <><button className="btn" onClick={() => setPage("index.md")}>Index</button><button className="btn" onClick={() => void openUrl(online)}>Open online documentation</button></>}
      <Markdown text={text || "Manual page unavailable."} navigate={navigate} />
      <button className="btn" onClick={close}>Close</button>
    </div>
  </div>;
}

export function offlineHelpMenu(open: (kind: "notes" | "manual") => void) {
  return [
    { label: "What's New", onClick: () => open("notes") },
    { label: "User Manual", onClick: () => open("manual") },
  ];
}

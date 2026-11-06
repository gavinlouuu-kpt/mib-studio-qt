import "./style.css";
import { driver } from "driver.js";
const el = <T extends HTMLElement = HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const value = (id: string) => el<HTMLInputElement>(id).value;
function say(message: string, error = false) {
  el("message").textContent = message;
  el("message").classList.toggle("error", error);
}
type Row = { id: string; name: string; status: string; error?: string };
type Candidate = {
  id: number;
  kind: string;
  points?: number[][];
  center?: number[];
  radius?: number;
  holes?: number[][][];
  layers?: string[];
};
type Inspection = {
  placement_contract?: number;
  open_paths?: number[][][];
  matching_groups?: number[][];
  chip_regions?: number[];
  manual_chip_rectangles?: number[][];
  cut_clearance_um?: number;
  generated_preview?: {
    dot_count: number;
    overview_png: string;
    detail_bounds: number[];
    detail_points: number[][];
    dot_radius_um: number;
    bounds?: number[];
    chip_dot_counts?: number[];
    exports?: {name: string; bytes: number}[];
    chip_details?: { detail_bounds: number[]; detail_points: number[][] }[];
  };
  region_candidates?: Candidate[];
  selected_regions?: number[];
  no_dot_regions?: number;
  generation_supported?: boolean;
  extent_um: number[];
  entity_count: number;
  source_units: string;
  converter: string | null;
  paths: number[][][];
  no_dot_paths?: number[][][];
  warnings: string[];
};
type Detail = Row & { inspection: Inspection | null };
type User = { email: string; role: string; active: number };
async function api<T = Record<string, string>>(
  path: string,
  options: RequestInit = {},
): Promise<T> {
  const r = await fetch(`/api${path}`, options);
  const b = await r.json();
  if (!r.ok) throw new Error(b.error ?? "Request failed");
  return b as T;
}
const post = (path: string, data: unknown) =>
  api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data),
  });
async function safely(f: () => Promise<void>) {
  try {
    await f();
  } catch (e) {
    say(e instanceof Error ? e.message : String(e), true);
  }
}
let selected: string | null = null;
let uploaded = false;
let ready = false;
let selectedRegions = new Set<number>();
let selectedChips = new Set<number>();
let manualChips: number[][] = [];
let activeInspection: Inspection | null = null;
let canConfirm = false;
function updateSelection() {
  el("selection-count").textContent =
    `${selectedChips.size + manualChips.length} chip boundaries · ${selectedRegions.size} fluid/exclusion faces selected`;
  const hasAreas =
    (activeInspection?.no_dot_regions ??
      activeInspection?.no_dot_paths?.length ??
      0) > 0 || selectedRegions.size > 0;
  el<HTMLButtonElement>("generate").disabled =
    !canConfirm ||
    !hasAreas ||
    !(selectedChips.size + manualChips.length) ||
    !el<HTMLInputElement>("confirmed").checked;
}

async function list() {
  const designs = await api<Row[]>("/designs");
  el("designs").replaceChildren();
  if (!designs.length)
    el("designs").textContent = "No designs yet. Upload your first drawing.";
  for (const d of designs) {
    const b = document.createElement("button");
    b.className = "design";
    b.textContent = d.name;
    const s = document.createElement("small");
    s.textContent = d.status.replaceAll("_", " ");
    b.append(s);
    b.onclick = () => void safely(() => open(d.id));
    el("designs").append(b);
  }
}
async function open(id: string) {
  selected = null;
  canConfirm = false;
  activeInspection = null;
  selectedRegions.clear();
  selectedChips.clear();
  manualChips = [];
  uploaded = false;
  ready = false;
  el("review").hidden = true;
  el("downloads").hidden = true;
  el<HTMLInputElement>("confirmed").checked = false;
  el<HTMLButtonElement>("generate").disabled = true;
  const d = await api<Detail>(`/designs/${id}`);
  selected = id;
  ready = d.status === "ready";
  if (d.status === "failed") {
    say(d.error ?? "Generation failed", true);
    const retry = document.createElement("button");
    retry.textContent = "Retry inspection";
    retry.onclick = () =>
      void safely(async () => {
        await post(`/designs/${id}/retry`, {});
        await list();
        await open(id);
      });
    el("message").append(" ", retry);
    return;
  }
  if (d.inspection) {
    uploaded = true;
    el("review").hidden = false;
    const i = d.inspection;
    activeInspection = i;
    selectedRegions = new Set(i.selected_regions ?? []);
    selectedChips = new Set(i.chip_regions ?? []);
    manualChips = [...(i.manual_chip_rectangles ?? [])];
    el<HTMLInputElement>("cut-clearance").value = String(
      i.cut_clearance_um ?? 200,
    );
    canConfirm =
      d.status === "awaiting_confirmation" &&
      i.generation_supported !== false &&
      i.placement_contract === 1;
    el("region-controls").hidden = false;
    for (const key of [
      "region-role",
      "toggle-region",
      "match-regions",
      "clear-regions",
      "add-manual-chip",
      "cut-clearance",
      "chip-x0",
      "chip-y0",
      "chip-x1",
      "chip-y1",
    ])
      el<HTMLInputElement>(key).disabled = d.status !== "awaiting_confirmation";
    el("reinspect").hidden = d.status !== "awaiting_confirmation";
    for (const key of [
      "region-role",
      "region-choice",
      "toggle-region",
      "match-regions",
      "focus-region",
    ])
      if (!i.region_candidates?.length)
        el<HTMLInputElement>(key).disabled = true;
    updateSelection();
    el("summary").textContent =
      `${d.name} · CAD extent ${i.extent_um.map((v: number) => v.toFixed(1)).join(" × ")} µm · ${i.entity_count} entities · source ${i.source_units}${i.converter ? " · DWG converted" : ""}`;
    el("dot-detail").hidden = !i.generated_preview;
    el("dot-chip").hidden = !i.generated_preview;
    el("dot-summary").textContent = i.generated_preview
      ? `${i.generated_preview.dot_count.toLocaleString()} generated dots${i.generated_preview.chip_dot_counts?.length ? ` across ${i.generated_preview.chip_dot_counts.length} chips (minimum ${Math.min(...i.generated_preview.chip_dot_counts).toLocaleString()} per chip)` : ""}. Overview shows dot occupancy; use Magnify actual dots for exact circles in a 1 mm detail.`
      : "";
    const svg = el("preview");
    svg.replaceChildren();
    const fitBox = () => {
      const x0 = Math.min(0, ...manualChips.map((r) => r[0])),
        y0 = Math.min(0, ...manualChips.map((r) => r[1]));
      const x1 = Math.max(i.extent_um[0], ...manualChips.map((r) => r[2])),
        y1 = Math.max(i.extent_um[1], ...manualChips.map((r) => r[3]));
      return [x0, i.extent_um[1] - y1, x1 - x0, y1 - y0];
    };
    const fitDrawing = () => svg.setAttribute("viewBox", fitBox().join(" "));
    fitDrawing();
    for (const points of [...(i.no_dot_paths ?? []), ...i.paths]) {
      const p = document.createElementNS(
        "http://www.w3.org/2000/svg",
        "polyline",
      );
      p.setAttribute(
        "points",
        points
          .map(([x, y]: number[]) => `${x},${i.extent_um[1] - y}`)
          .join(" "),
      );
      p.setAttribute(
        "fill",
        i.no_dot_paths?.includes(points) ? "#b9e1d4" : "none",
      );
      p.setAttribute("stroke", "#326e60");
      p.setAttribute("stroke-width", "1.2");
      p.setAttribute("vector-effect", "non-scaling-stroke");
      svg.append(p);
    }
    let overview: SVGElement | undefined;
    let detail: SVGElement | undefined;
    const generated = i.generated_preview;
    if (generated) {
      overview = document.createElementNS(
        "http://www.w3.org/2000/svg",
        "image",
      );
      overview.setAttribute(
        "href",
        `data:image/png;base64,${generated.overview_png}`,
      );
      const [gx0, gy0, gx1, gy1] = generated.bounds ?? [0, 0, ...i.extent_um];
      overview.setAttribute("x", String(gx0));
      overview.setAttribute("y", String(i.extent_um[1] - gy1));
      overview.setAttribute("width", String(gx1 - gx0));
      overview.setAttribute("height", String(gy1 - gy0));
      overview.style.pointerEvents = "none";
      svg.prepend(overview);
      detail = document.createElementNS("http://www.w3.org/2000/svg", "g");
      detail.setAttribute("data-dot-detail", "true");
      detail.style.pointerEvents = "none";
      detail.style.display = "none";
      const drawDetail = (ps: number[][]) => {
        detail!.replaceChildren();
        for (const [x, y] of ps) {
          const dot = document.createElementNS(
            "http://www.w3.org/2000/svg",
            "circle",
          );
          dot.setAttribute("cx", String(x));
          dot.setAttribute("cy", String(i.extent_um[1] - y));
          dot.setAttribute("r", String(generated.dot_radius_um));
          dot.setAttribute("fill", "#145ab4");
          detail!.append(dot);
        }
      };
      drawDetail(generated.detail_points);
      const chipChoice = el<HTMLSelectElement>("dot-chip");
      chipChoice.replaceChildren();
      (generated.chip_details ?? [generated]).forEach((_, n) => {
        const option = document.createElement("option");
        option.value = String(n);
        option.textContent = `Chip ${n + 1}`;
        chipChoice.append(option);
      });
      svg.append(detail);
      el("dot-detail").onclick = () => {
        const view =
          generated.chip_details?.[Number(chipChoice.value)] ?? generated;
        drawDetail(view.detail_points);
        const [x0, y0, x1, y1] = view.detail_bounds;
        svg.setAttribute(
          "viewBox",
          `${x0} ${i.extent_um[1] - y1} ${x1 - x0} ${y1 - y0}`,
        );
        svg.style.maxWidth = "600px";
        svg.style.height = "auto";
        svg.style.aspectRatio = "1 / 1";
        overview!.style.display = "none";
        detail!.style.display = "";
      };
    }
    el("reset-view").onclick = () => {
      fitDrawing();
      if (overview) overview.style.display = "";
      if (detail) detail.style.display = "none";
      svg.style.maxWidth = "";
      svg.style.height = "";
      svg.style.aspectRatio = "";
    };
    svg.onwheel = (event) => {
      event.preventDefault();
      if (overview) overview.style.display = "";
      if (detail) detail.style.display = "none";
      svg.style.maxWidth = "";
      svg.style.height = "";
      svg.style.aspectRatio = "";
      const box = svg.getAttribute("viewBox")!.split(" ").map(Number);
      const rect = svg.getBoundingClientRect();
      const width = Math.min(
        fitBox()[2] * 2,
        Math.max(50, box[2] * (event.deltaY < 0 ? 0.7 : 1.4)),
      );
      const height = (width * box[3]) / box[2];
      const fx = (event.clientX - rect.left) / rect.width,
        fy = (event.clientY - rect.top) / rect.height;
      svg.setAttribute(
        "viewBox",
        `${box[0] + fx * (box[2] - width)} ${box[1] + fy * (box[3] - height)} ${width} ${height}`,
      );
    };
    const area = (r: Candidate) =>
      r.kind === "circle"
        ? Math.PI * r.radius! ** 2
        : Math.abs(
            r.points!.reduce((a, p, n, ps) => {
              const q = ps[(n + 1) % ps.length];
              return a + p[0] * q[1] - q[0] * p[1];
            }, 0) / 2,
          );
    for (const ps of i.open_paths ?? []) {
      const line = document.createElementNS(
        "http://www.w3.org/2000/svg",
        "polyline",
      );
      line.setAttribute(
        "points",
        ps.map(([x, y]) => `${x},${i.extent_um[1] - y}`).join(" "),
      );
      line.setAttribute("stroke", "#d32222");
      line.setAttribute("stroke-width", "4");
      line.setAttribute("vector-effect", "non-scaling-stroke");
      line.style.pointerEvents = "none";
      svg.append(line);
    }
    const shapes = new Map<number, SVGElement>();
    const paint = (id: number) => {
      const shape = shapes.get(id);
      const r = i.region_candidates?.find((c) => c.id === id);
      if (r?.kind === "polygon") {
        const rings = selectedChips.has(id)
          ? [r.points!]
          : [r.points!, ...(r.holes ?? [])];
        shape?.setAttribute(
          "d",
          rings
            .map(
              (ps) =>
                ps
                  .map(
                    ([x, y], n) => `${n ? "L" : "M"}${x},${i.extent_um[1] - y}`,
                  )
                  .join(" ") + " Z",
            )
            .join(" "),
        );
      }
      shape?.setAttribute(
        "fill",
        selectedRegions.has(id) ? "#ef985a80" : "transparent",
      );
      shape?.setAttribute(
        "stroke",
        selectedChips.has(id) ? "#176bd6" : "#64748b",
      );
      shape?.setAttribute("stroke-width", selectedChips.has(id) ? "3" : "0.5");
    };
    const toggle = (id: number) => {
      if (d.status !== "awaiting_confirmation") return;
      if (!i.region_candidates?.some((r) => r.id === id)) return;
      const chips = value("region-role") === "chip";
      const selection = chips ? selectedChips : selectedRegions;
      selection.has(id) ? selection.delete(id) : selection.add(id);
      (chips ? selectedRegions : selectedChips).delete(id);
      paint(id);
      el<HTMLInputElement>("confirmed").checked = false;
      updateSelection();
    };
    const select = el<HTMLSelectElement>("region-choice");
    select.replaceChildren();
    for (const r of [...(i.region_candidates ?? [])].sort(
      (a, b) => area(b) - area(a),
    )) {
      const shape = document.createElementNS(
        "http://www.w3.org/2000/svg",
        r.kind === "circle" ? "circle" : "path",
      );
      if (r.kind === "circle") {
        shape.setAttribute("cx", String(r.center![0]));
        shape.setAttribute("cy", String(i.extent_um[1] - r.center![1]));
        shape.setAttribute("r", String(r.radius));
      } else {
        const ring = (ps: number[][]) =>
          ps
            .map(([x, y], n) => `${n ? "L" : "M"}${x},${i.extent_um[1] - y}`)
            .join(" ") + " Z";
        shape.setAttribute(
          "d",
          [r.points!, ...(r.holes ?? [])].map(ring).join(" "),
        );
        shape.setAttribute("fill-rule", "evenodd");
      }
      shape.setAttribute(
        "fill",
        selectedRegions.has(r.id) ? "#ef985a80" : "transparent",
      );
      shape.setAttribute("stroke", "#64748b");
      shape.setAttribute("stroke-width", "0.5");
      shape.setAttribute("vector-effect", "non-scaling-stroke");
      shape.setAttribute("data-region", String(r.id));
      const title = document.createElementNS(
        "http://www.w3.org/2000/svg",
        "title",
      );
      title.textContent = `Area ${r.id + 1}: assign using the selected role`;
      shape.append(title);
      shape.onclick = () => toggle(r.id);
      shapes.set(r.id, shape);
      paint(r.id);
      svg.append(shape);
      const option = document.createElement("option");
      option.value = String(r.id);
      option.textContent = `Area ${r.id + 1} · ${area(r).toFixed(0)} µm² · ${r.layers?.length ? "layer " + r.layers.join(", ") : "assembled face"}${r.holes?.length ? " · holes preserved" : ""}`;
      select.append(option);
    }
    el("toggle-region").onclick = () => toggle(Number(select.value));
    el("match-regions").onclick = () => {
      const id = Number(select.value);
      const ids = i.matching_groups?.find((group) => group.includes(id)) ?? [
        id,
      ];
      const selection =
        value("region-role") === "chip" ? selectedChips : selectedRegions;
      for (const n of ids) if (!selection.has(n)) toggle(n);
      say(
        `${ids.length} translated copies assigned. Geometry matches within 0.01 µm; verify the role and coverage on every chip.`,
      );
    };
    el("focus-region").onclick = () => {
      if (overview) overview.style.display = "";
      if (detail) detail.style.display = "none";
      svg.style.maxWidth = "";
      svg.style.height = "";
      svg.style.aspectRatio = "";
      const r = i.region_candidates?.find((c) => c.id === Number(select.value));
      if (!r) return;
      const ps =
        r.kind === "circle"
          ? [
              [r.center![0] - r.radius!, r.center![1] - r.radius!],
              [r.center![0] + r.radius!, r.center![1] + r.radius!],
            ]
          : r.points!;
      const xs = ps.map((p) => p[0]),
        ys = ps.map((p) => p[1]);
      const x0 = Math.min(...xs),
        x1 = Math.max(...xs),
        y0 = Math.min(...ys),
        y1 = Math.max(...ys);
      svg.setAttribute(
        "viewBox",
        `${x0 - 200} ${i.extent_um[1] - y1 - 200} ${x1 - x0 + 400} ${y1 - y0 + 400}`,
      );
    };
    el("clear-regions").onclick = () => {
      if (d.status !== "awaiting_confirmation") return;
      selectedRegions.clear();
      selectedChips.clear();
      manualChips = [];
      fitDrawing();
      for (const id of shapes.keys()) paint(id);
      svg.querySelectorAll("[data-manual-chip]").forEach((e) => e.remove());
      el<HTMLInputElement>("confirmed").checked = false;
      updateSelection();
    };
    const drawManual = (r: number[]) => {
      const rect = document.createElementNS(
        "http://www.w3.org/2000/svg",
        "rect",
      );
      rect.setAttribute("x", String(r[0]));
      rect.setAttribute("y", String(i.extent_um[1] - r[3]));
      rect.setAttribute("width", String(r[2] - r[0]));
      rect.setAttribute("height", String(r[3] - r[1]));
      rect.setAttribute("fill", "none");
      rect.setAttribute("stroke", "#176bd6");
      rect.setAttribute("stroke-width", "3");
      rect.setAttribute("vector-effect", "non-scaling-stroke");
      rect.setAttribute("data-manual-chip", "true");
      rect.style.pointerEvents = "none";
      svg.append(rect);
    };
    manualChips.forEach(drawManual);
    el("add-manual-chip").onclick = () => {
      if (d.status !== "awaiting_confirmation") return;
      const r = ["chip-x0", "chip-y0", "chip-x1", "chip-y1"].map((k) =>
        Number(value(k)),
      );
      if (
        !r.every(Number.isFinite) ||
        !(
          -150000 <= r[0] &&
          r[0] < r[2] &&
          r[2] <= 150000 &&
          -150000 <= r[1] &&
          r[1] < r[3] &&
          r[3] <= 150000
        ) ||
        selectedChips.size + manualChips.length >= 100
      ) {
        say(
          "Enter a valid rectangle in micrometres, within the supported coordinate range. It may extend beyond the CAD geometry.",
          true,
        );
        return;
      }
      manualChips.push(r);
      drawManual(r);
      fitDrawing();
      el<HTMLInputElement>("confirmed").checked = false;
      updateSelection();
    };
    el("cut-clearance").oninput = () => {
      el<HTMLInputElement>("confirmed").checked = false;
      updateSelection();
    };
    el("reinspect").onclick = () =>
      void safely(async () => {
        await post(`/designs/${id}/retry`, {});
        await list();
        await open(id);
      });
    const warnings = document.createElement("ul");
    for (const message of i.warnings) {
      const item = document.createElement("li");
      item.textContent = message;
      warnings.append(item);
    }
    el("warnings").replaceChildren(warnings);
    el<HTMLInputElement>("confirmed").checked = false;
    el<HTMLInputElement>("confirmed").disabled =
      d.status !== "awaiting_confirmation";
    el<HTMLButtonElement>("generate").disabled = true;
  }
  if (ready) {
    el("downloads").hidden = false;
    el("links").replaceChildren();
    const exports = d.inspection?.generated_preview?.exports;
    const labels: Record<string, string> = {
      "design_with_dotgrid.dwg": "Device + dots · DWG",
      "dotgrid_layer.oas": "Dots only · OASIS",
      "design_with_dotgrid.dxf": "Device + dots · DXF (large compatibility file)",
      "dotgrid_layer.gds": "Dots only · GDS (compatibility file)",
    };
    const compatibility = document.createElement("details");
    const heading = document.createElement("summary");
    heading.textContent = "Other file formats";
    compatibility.append(heading);
    const available = exports ?? [
      {name: "design_with_dotgrid.dxf", bytes: 0},
      {name: "dotgrid_layer.gds", bytes: 0},
    ];
    for (const {name, bytes} of available) {
      if (!labels[name]) continue;
      const a = document.createElement("a");
      a.href = `/api/designs/${id}/files/${name}`;
      a.textContent = labels[name] + (bytes ? ` · ${(bytes/1e6).toFixed(2)} MB` : "");
      if (exports && ["design_with_dotgrid.dxf", "dotgrid_layer.gds"].includes(name))
        compatibility.append(a);
      else el("links").append(a);
    }
    if (exports) el("links").append(compatibility);
    say(
      "Your device and proprietary dots are ready to download. Review the geometry before fabrication.",
    );
  } else if (d.status === "needs_review") {
    say(
      "This older output lacks post-cut boundary checks. Upload the original CAD to create a reviewed revision.",
    );
  } else
    say(
      `Design status: ${d.status.replaceAll("_", " ")}. Use Refresh to check progress.`,
    );
}
el("upload-form").onsubmit = (e) => {
  e.preventDefault();
  void safely(async () => {
    const f = el<HTMLInputElement>("file").files?.[0];
    if (!f) throw new Error("Choose a CAD file");
    const ext = f.name.split(".").pop()?.toLowerCase();
    if (!["dxf", "dwg"].includes(ext ?? ""))
      throw new Error("Choose DXF or DWG");
    if (f.size > 10 * 1024 * 1024) throw new Error("File exceeds 10 MiB");
    el<HTMLButtonElement>("upload").disabled = true;
    try {
      const q = new URLSearchParams({
        name: value("name"),
        extension: ext!,
        units: value("units"),
      });
      const r = await api(`/designs?${q}`, {
        method: "POST",
        body: f,
        headers: { "Content-Type": "application/octet-stream" },
      });
      selected = r.id;
      await list();
      say(
        "Uploaded. Inspection is queued. Refresh to view the converted geometry.",
      );
    } finally {
      el<HTMLButtonElement>("upload").disabled = false;
    }
  });
};
el("refresh").onclick = () =>
  void safely(async () => {
    await list();
    if (selected) await open(selected);
  });
el("confirmed").onchange = updateSelection;
el("generate").onclick = () =>
  void safely(async () => {
    if (!selected) return;
    el<HTMLButtonElement>("generate").disabled = true;
    try {
      await post(`/designs/${selected}/confirm`, {
        confirmed: el<HTMLInputElement>("confirmed").checked,
        selected_regions: [...selectedRegions],
        chip_regions: [...selectedChips],
        manual_chip_rectangles: manualChips,
        cut_clearance_um: Number(value("cut-clearance")),
      });
      await list();
      await open(selected);
    } catch (e) {
      updateSelection();
      throw e;
    }
  });
async function members() {
  el("member-list").replaceChildren();
  for (const m of await api<User[]>("/members")) {
    const li = document.createElement("li");
    li.textContent = `${m.email} · ${m.role} · ${m.active ? "active" : "revoked"}`;
    if (m.role === "member" && m.active) {
      const b = document.createElement("button");
      b.textContent = "Revoke";
      b.onclick = () =>
        void safely(async () => {
          await api("/members", {
            method: "PATCH",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ email: m.email }),
          });
          await members();
        });
      li.append(b);
    }
    el("member-list").append(li);
  }
}
el("invite-form").onsubmit = (e) => {
  e.preventDefault();
  void safely(async () => {
    const r = await post("/members", { email: value("invite-email") });
    say(r.message);
    await members();
  });
};
el("feedback-form").onsubmit = (e) => {
  e.preventDefault();
  void safely(async () => {
    await post("/feedback", { text: value("feedback-text") });
    say("Feedback saved. Thank you.");
    el<HTMLTextAreaElement>("feedback-text").value = "";
  });
};
el("region-tour").onclick = () => {
  if (!activeInspection || el("review").hidden) {
    say("Open an imported drawing first.");
    return;
  }
  const tour = driver({
    showProgress: true,
    allowClose: true,
    doneBtnText: "Finish",
    steps: [
      {
        element: "#preview",
        popover: {
          title: "Inspect your actual drawing",
          description:
            "Scroll to zoom; Fit drawing resets the view. The imported device geometry is preserved. Closed areas are not automatically classified as channels.",
        },
      },
      {
        element: "#region-controls",
        popover: {
          title: "Separate chip edges from fluid",
          description:
            "Close the tour. Assign the final cut outline in blue, then fluid faces in orange. Holes in fluid faces remain solid islands. If the boundary is absent, define a rectangle by its dimensions. Restart the guide to continue.",
          onNextClick: () => {
            if (
              (!selectedRegions.size &&
                !(activeInspection?.no_dot_regions ?? 0)) ||
              !(selectedChips.size + manualChips.length)
            ) {
              say(
                "Choose a chip boundary and its fluid exclusions before continuing.",
              );
              return;
            }
            tour.moveNext();
          },
        },
      },
      {
        element: "#confirmed",
        popover: {
          title: "Verify complete coverage",
          description:
            "Check physical units and all shaded interiors. Open fragments need review. Changing a selection clears confirmation. Dots must remain inside every selected cut boundary and outside all orange fluid faces. Confirm the cutting clearance. Every chip must retain dots; this does not prove optical decoding.",
        },
      },
      {
        element: "#feedback-form",
        popover: {
          title: "Feedback checkpoint",
          description:
            "Report missing geometry or unclear area selection here. Do not confirm a drawing whose channel coverage you cannot verify.",
        },
      },
    ],
  });
  tour.drive();
};
el("tour").onclick = () => {
  const tour = driver({
    showProgress: true,
    allowClose: true,
    doneBtnText: "Finish",
    steps: [
      {
        element: "#upload-card",
        popover: {
          title: "Start with a device drawing",
          description:
            "Choose DXF or DWG, name the design and explicitly select its units. You can close this tour and restart it at any time.",
        },
      },
      {
        element: "#designs-card",
        popover: {
          title: "Inspect before generation",
          description:
            "Upload a file, then refresh and open its design when inspection finishes.",
          onNextClick: () => {
            if (!uploaded) {
              say(
                "Upload, refresh and open an inspected drawing before continuing. Close the tour to perform these steps.",
              );
              return;
            }
            tour.moveNext();
          },
        },
      },
      {
        element: "#review",
        popover: {
          title: "Confirm the converted geometry",
          description:
            "Check dimensions, orientation, units and shaded no-dot areas. Every channel and port must be covered before confirming.",
          onNextClick: () => {
            if (!ready) {
              say(
                "Confirm and generate, then refresh and open the completed design before continuing. Close the tour to perform these steps.",
              );
              return;
            }
            tour.moveNext();
          },
        },
      },
      {
        element: "#downloads",
        popover: {
          title: "Device and dots",
          description:
            "Use the compact device + dots DWG when available, or the dots-only OASIS. File sizes are shown before download; DXF and GDS remain under Other file formats. The dot technology is proprietary.",
        },
      },
      {
        element: "#feedback",
        popover: {
          title: "Feedback checkpoint",
          description:
            "Tell us if a conversion, preview or export needs improvement. Feedback is saved to your workspace.",
        },
      },
    ],
    onHighlightStarted: (element) => {
      if (!element || (element as HTMLElement).hidden) {
        tour.destroy();
        say(
          "That tour step is not available yet. Complete the preceding step, then restart the tour.",
        );
      }
    },
  });
  tour.drive();
};
void safely(async () => {
  const me = await api<User>("/me");
  el("identity").textContent = me.email;
  await list();
  if (me.role === "admin") {
    el("members").hidden = false;
    await members();
  }
});

let showDemoDots = true;
el("demo-toggle").onclick = () => {
  showDemoDots = !showDemoDots;
  el<HTMLImageElement>("demo-image").src = showDemoDots
    ? "/demo/after.svg"
    : "/demo/before.svg";
  el("demo-toggle").textContent = showDemoDots
    ? "Hide added dots"
    : "Show added dots";
  el("demo-toggle").setAttribute("aria-pressed", String(showDemoDots));
  el<HTMLImageElement>("demo-image").alt = showDemoDots
    ? "Actual generated dots and original device walls"
    : "Original device walls without dot grid";
};
el("demo-load").onclick = () =>
  void safely(async () => {
    const r = await fetch("/demo/sorter-concept.dxf");
    if (!r.ok) throw new Error("Sample CAD unavailable");
    const transfer = new DataTransfer();
    transfer.items.add(
      new File([await r.blob()], "sorter-concept.dxf", {
        type: "application/octet-stream",
      }),
    );
    el<HTMLInputElement>("file").files = transfer.files;
    el<HTMLInputElement>("name").value = "Microfluidic sorter concept";
    el<HTMLSelectElement>("units").value = "mm";
    el("upload-card").scrollIntoView({ behavior: "smooth", block: "center" });
    say(
      "Sample loaded in millimetres. Click Upload & inspect to process the real CAD.",
    );
  });
el("demo-tour").onclick = () => {
  const tour = driver({
    showProgress: true,
    allowClose: true,
    steps: [
      {
        element: "#demo-device",
        popover: {
          title: "Start with recognizable device CAD",
          description:
            "The ports, mixer, observation region and outlets belong to the source drawing. The orange box marks the region magnified alongside it.",
        },
      },
      {
        element: "#demo-detail",
        popover: {
          title: "See exactly what gets added",
          description:
            "Orange dots come from the actual generated DXF, not a decorative grid. Close the tour and toggle Hide added dots to compare against the unchanged device walls; restart here any time.",
        },
      },
      {
        element: "#demo-output",
        popover: {
          title: "Download device and dots",
          description:
            "The combined DXF preserves the device with added proprietary dots. The GDS contains only the dots. Channels and ports are excluded.",
        },
      },
      {
        element: "#demo-load",
        popover: {
          title: "Try the real workflow",
          description:
            "Close this tour, choose Use this sample CAD, then Upload & inspect. Check the millimetre-to-micrometre conversion, confirm, and generate your own review outputs.",
        },
      },
    ],
    onHighlightStarted: (element) => {
      if (!element) {
        tour.destroy();
        say("Example unavailable; refresh and retry.");
      }
    },
  });
  tour.drive();
};
void safely(async () => {
  const r = await fetch("/demo/sample.json");
  if (!r.ok) throw new Error("Sample metadata unavailable");
  const sample = await r.json();
  el("demo-result").textContent =
    `Verified example: ${Number(sample.dot_count).toLocaleString()} generated dots · proprietary dot technology. These illustrations are cropped from its actual exported DXF.`;
});

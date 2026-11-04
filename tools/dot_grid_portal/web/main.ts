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
type Inspection = {
  extent_um: number[];
  entity_count: number;
  source_units: string;
  converter: string | null;
  paths: number[][][];
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
    el("summary").textContent =
      `${d.name} · ${i.extent_um.map((v: number) => v.toFixed(1)).join(" × ")} µm · ${i.entity_count} entities · source ${i.source_units}${i.converter ? " · DWG converted" : ""}`;
    const svg = el("preview");
    svg.replaceChildren();
    svg.setAttribute("viewBox", `0 0 ${i.extent_um[0]} ${i.extent_um[1]}`);
    for (const points of i.paths) {
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
      p.setAttribute("fill", "none");
      p.setAttribute("stroke", "#326e60");
      p.setAttribute("stroke-width", "1.2");
      p.setAttribute("vector-effect", "non-scaling-stroke");
      svg.append(p);
    }
    el("warnings").textContent = i.warnings.join(" ");
    el<HTMLInputElement>("confirmed").checked = false;
    el<HTMLInputElement>("confirmed").disabled =
      d.status !== "awaiting_confirmation";
    el<HTMLButtonElement>("generate").disabled = true;
  }
  if (ready) {
    el("downloads").hidden = false;
    el("links").replaceChildren();
    for (const [name, label] of [
      ["design_with_dotgrid.dxf", "Device + dots · DXF"],
      ["dotgrid_layer.gds", "Dots only · GDS"],
      ["mib-map.zip", "MIB Studio map · ZIP"],
    ]) {
      const a = document.createElement("a");
      a.href = `/api/designs/${id}/files/${name}`;
      a.textContent = label;
      el("links").append(a);
    }
    say(
      "Generation complete. Registered files are ready; fabrication approval is separate.",
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
el("confirmed").onchange = () => {
  el<HTMLButtonElement>("generate").disabled =
    !el<HTMLInputElement>("confirmed").checked;
};
el("generate").onclick = () =>
  void safely(async () => {
    if (!selected) return;
    el<HTMLButtonElement>("generate").disabled = true;
    try {
      await post(`/designs/${selected}/confirm`, {
        confirmed: el<HTMLInputElement>("confirmed").checked,
      });
      await list();
      await open(selected);
    } catch (e) {
      el<HTMLButtonElement>("generate").disabled =
        !el<HTMLInputElement>("confirmed").checked;
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
            "Check dimensions, orientation and units. Only a confirmed preview can be generated.",
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
          title: "Three distinct outputs",
          description:
            "Combined DXF, dots-only GDS, and the registered MIB Studio map package. These are not fabrication approval.",
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
        element: "#demo-map",
        popover: {
          title: "Give MIB Studio a localization map",
          description:
            "The map package pairs the unique pattern identity with coordinates. Synthetic decoding passed; this does not claim a real camera or fabricated chip has been tested.",
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
    `Verified example: ${Number(sample.dot_count).toLocaleString()} generated dots · 30 µm pitch · 12 µm diameter · codec ${sample.codec_contract}. These illustrations are cropped from its actual exported DXF.`;
});

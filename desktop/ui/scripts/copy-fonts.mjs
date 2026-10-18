// Ship the Inter faces as real files (Vite's library mode would inline them
// as base64): dist/fonts/*.woff2 plus dist/fonts.css. Hosts import
// "@mib/ui/fonts.css" next to "@mib/ui/style.css".
import { cpSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
cpSync(resolve(root, "src/fonts"), resolve(root, "dist/fonts"), { recursive: true });
const faces = readFileSync(resolve(root, "src/styles/fonts.css"), "utf8").replaceAll("../fonts/", "./fonts/");
writeFileSync(resolve(root, "dist/fonts.css"), faces);

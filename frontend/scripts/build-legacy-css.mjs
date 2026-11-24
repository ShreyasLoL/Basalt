import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Scanner } from "@tailwindcss/oxide";
import { compile } from "@tailwindcss/node";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const frontendDirectory = path.resolve(scriptDirectory, "..");
const inputPath = path.join(frontendDirectory, "legacy.css");
const outputPath = path.resolve(frontendDirectory, "..", "web", "legacy.css");
const htmlPath = path.resolve(frontendDirectory, "..", "web", "index.html");
const source = await readFile(inputPath, "utf8");
const html = await readFile(htmlPath, "utf8");
if (!html.includes('<script src="/legacy.js" defer></script>')) {
  throw new Error("The legacy fallback must load its same-origin external script");
}
if (/<script(?![^>]*\bsrc=)[^>]*>[\s\S]*?<\/script>/i.test(html)) {
  throw new Error("The legacy fallback cannot contain inline scripts");
}
if (html.includes("https://cdn.tailwindcss.com")) {
  throw new Error("The legacy fallback cannot load executable CDN code");
}
const compiler = await compile(source, {
  base: frontendDirectory,
  from: inputPath,
  onDependency: () => {},
});
const scanner = new Scanner({ sources: compiler.sources });
const candidates = scanner.scan();
await writeFile(outputPath, compiler.build(candidates), "utf8");

import path from "node:path";
import { createRequire } from "node:module";

const [esbuildPackage, worldDir, outDir, ...entryNames] = process.argv.slice(2);
const require = createRequire(import.meta.url);
const esbuild = require(esbuildPackage);
const sceneDir = path.join(worldDir, "scene");
const publicDir = path.dirname(worldDir);
const entries = entryNames.map((name) => path.join(worldDir, name));
const posix = (value) => value.split(path.sep).join("/");
const bundled = (file) => entries.includes(file) || file.startsWith(sceneDir + path.sep);

await esbuild.build({
  entryPoints: entries,
  outbase: worldDir,
  outdir: outDir,
  bundle: true,
  splitting: true,
  format: "esm",
  minify: true,
  sourcemap: "external",
  sourcesContent: true,
  charset: "utf8",
  legalComments: "none",
  logLevel: "error",
  chunkNames: "scene/chunks/[name]-[hash]",
  metafile: false,
  plugins: [
    {
      name: "world-shared-modules",
      setup(build) {
        build.onResolve({ filter: /^\.{1,2}\// }, (args) => {
          if (!args.importer) return undefined;
          const resolved = path.resolve(path.dirname(args.importer), args.path);
          if (bundled(resolved)) return undefined;
          const inWorld = !path.relative(worldDir, resolved).startsWith("..");
          const url = inWorld
            ? `/world/${posix(path.relative(worldDir, resolved))}`
            : `/${posix(path.relative(publicDir, resolved))}`;
          return { path: url, external: true };
        });
      },
    },
  ],
});

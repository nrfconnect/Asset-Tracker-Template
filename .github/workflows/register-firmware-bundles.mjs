import fs, { createWriteStream } from "node:fs";
import { readFile } from "node:fs/promises";
import { mkdtemp } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import yazl from "yazl";
import { nameRegEx } from "./lib/nameRegEx.mjs";

const version = (process.argv[process.argv.length - 1] ?? "").trim();
const filenameRegEx = nameRegEx(version);

console.log(`Publishing release version`, version);

const assets = fs
  .readdirSync(process.cwd())
  .filter((name) => filenameRegEx.test(name));

if (assets.length === 0) {
  console.error(`No assets found for release ${version}!`);
  process.exit(1);
}

for (const asset of assets) {
  const {
    groups: { configuration },
  } = filenameRegEx.exec(asset);

  const fwversion = `${version}${
    configuration !== undefined ? `+${configuration}` : ""
  }`;
  const fwName = `asset-tracker-template ${fwversion}`;
  console.log(fwName);
  const manifest = {
    name: fwName,
    description: `Firmware update for the Thingy:91 X (${
      configuration ?? "default"
    })`,
    fwversion,
    "format-version": 1,
    files: [
      {
        file: asset,
        type: "application",
        size: fs.statSync(asset).size,
      },
    ],
  };

  console.log(JSON.stringify(manifest, null, 2));

  const zipfile = new yazl.ZipFile();
  zipfile.addBuffer(
    Buffer.from(JSON.stringify(manifest, null, 2), "utf-8"),
    "manifest.json"
  );

  const tempDir = await mkdtemp(path.join(os.tmpdir(), asset));

  zipfile.addFile(path.join(process.cwd(), asset), asset);

  const zipFileName = path.join(tempDir, `${asset}.zip`);

  await new Promise((resolve) => {
    zipfile.outputStream
      .pipe(createWriteStream(zipFileName))
      .on("close", () => {
        resolve();
      });
    zipfile.end();
  });

  const createFirmwareRes = await fetch(
    "https://api.nrfcloud.com/v1/firmwares",
    {
      method: "POST",
      headers: {
        Authorization: `Bearer ${process.env.NRF_CLOUD_API_KEY}`,
        "Content-Type": "application/zip",
      },
      body: await readFile(zipFileName),
    }
  );

  if (createFirmwareRes.status !== 200) {
    console.error(
      `Failed to upload firmware bundle: ${await createFirmwareRes.text()}`
    );
  } else {
    console.log(JSON.stringify(await createFirmwareRes.json(), null, 2));
  }
}

import { Octokit } from "@octokit/rest";
import fs from "node:fs";
import { Readable } from "stream";
import { finished } from "stream/promises";

const owner = process.env.OWNER ?? "nrfconnect";
const repo = process.env.REPO ?? "Asset-Tracker-Template";
const version = process.argv[process.argv.length - 1];

console.log(`Release version`, version);

const octokit = new Octokit({
  auth: process.env.GITHUB_TOKEN,
});

console.log(`Repository: ${owner}/${repo}`);

const { data: release } = await octokit.rest.repos.getReleaseByTag({
  repo,
  owner,
  tag: version,
});

if (release === undefined) {
  console.error(`Release for ${version} not found!`);
  console.debug(`Got: ${releases.map(({ name }) => name).join(", ")}!`);
  process.exit(1);
}

for (const { name, browser_download_url } of release.assets) {
  const res = await fetch(browser_download_url, {
    headers: {
      Accept: "application/octet-stream",
      Authorization: `Bearer ${process.env.GITHUB_TOKEN}`,
    },
  });

  const body = Readable.fromWeb(res.body);
  const download_write_stream = fs.createWriteStream(name);
  await finished(body.pipe(download_write_stream));
  console.log(name, "downloaded");
}

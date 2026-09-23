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
  process.exit(1);
}

// Don't rely on the assets embedded in the release-by-tag response: that
// endpoint hits stale replicas that intermittently return an empty `assets`
// array, and it caps the list at 30 entries. Fetch the assets by release id
// with pagination instead, which reads reliably and returns them all.
const assets = await octokit.paginate(octokit.rest.repos.listReleaseAssets, {
  owner,
  repo,
  release_id: release.id,
  per_page: 100,
});

if (assets.length === 0) {
  console.error(`Release ${version} (id ${release.id}) has no assets!`);
  process.exit(1);
}

for (const { name, browser_download_url } of assets) {
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

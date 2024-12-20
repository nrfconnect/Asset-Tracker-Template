import { nameRegEx } from "./nameRegEx.mjs";
import assert from "node:assert/strict";
import { describe, it } from "node:test";

describe("nameRegEx", () => {
  it("should match a valid filename without configuration", () =>
    assert.ok(
      nameRegEx("1.0.0").test(
        "hello.nrfcloud.com-1.0.0-thingy91x-nrf91-update-signed.bin"
      )
    ));

  it("should match a valid filename with configuration", () =>
    assert.ok(
      nameRegEx("1.0.0").test(
        "hello.nrfcloud.com-1.0.0+debug-thingy91x-nrf91-update-signed.bin"
      )
    ));

  it("should not match an invalid filename", () => {
    const version = "1.0.0";
    const regex = nameRegEx(version);
    const filename = "invalid-filename.bin";
    assert.ok(!regex.test(filename));
  });

  it("should not match a filename with a different version", () => {
    const version = "1.0.0";
    const regex = nameRegEx(version);
    const filename =
      "hello.nrfcloud.com-2.0.0-thingy91x-nrf91-update-signed.bin";
    assert.ok(!regex.test(filename));
  });
});

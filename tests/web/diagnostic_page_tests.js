"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const pagePath = process.argv[2];
if (!pagePath) {
  throw new Error("usage: node diagnostic_page_tests.js DIAGNOSTIC_HTML");
}

const page = fs.readFileSync(pagePath, "utf8");
const match = page.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(match, "diagnostic page must contain one inline script");

function metrics(state) {
  return {
    id: "cam01",
    state,
    ffmpeg_pid: state === "RUNNING" ? 4321 : null,
    consecutive_failures: state === "RUNNING" ? 0 : 12,
    total_restarts: 12,
    input: {status: "unavailable"},
    progress: {status: "unavailable"},
    process_metrics: {status: "unavailable"}
  };
}

async function settle() {
  await new Promise((resolve) => setImmediate(resolve));
  await new Promise((resolve) => setImmediate(resolve));
}

async function loadPage(search, responses) {
  const elements = new Map();
  let intervalCallback = null;
  let responseIndex = 0;

  function element(id) {
    if (!elements.has(id)) {
      const current = {textContent: "", assignedUrls: []};
      Object.defineProperty(current, "src", {
        set(value) {
          current.assignedUrls.push(value);
        }
      });
      elements.set(id, current);
    }
    return elements.get(id);
  }

  const context = vm.createContext({
    console,
    URL,
    URLSearchParams,
    location: {
      pathname: "/view/cam01",
      search
    },
    document: {
      getElementById: element
    },
    fetch: async () => {
      const response = responses[Math.min(responseIndex, responses.length - 1)];
      responseIndex += 1;
      if (response instanceof Error) {
        throw response;
      }
      return {
        ok: true,
        json: async () => response
      };
    },
    setInterval: (callback, milliseconds) => {
      assert.equal(milliseconds, 1000);
      intervalCallback = callback;
      return 1;
    }
  });

  element("video");
  vm.runInContext(match[1], context, {filename: pagePath});
  await settle();
  return {
    context,
    elements,
    async refresh() {
      assert.ok(intervalCallback, "page must register its refresh interval");
      await intervalCallback();
      await settle();
    }
  };
}

async function main() {
  const validation = await loadPage("?media_host=192.168.1.45", [metrics("BACKOFF")]);
  assert.equal(vm.runInContext("normalizeMediaHost('192.168.1.45')", validation.context),
               "192.168.1.45");
  assert.equal(vm.runInContext("normalizeMediaHost('board.local')", validation.context),
               "board.local");
  assert.equal(vm.runInContext("normalizeMediaHost('2001:db8::1')", validation.context),
               "[2001:db8::1]");
  for (const invalid of [
    "", " 192.168.1.45", "192.168.1.999", "192.168.01.45",
    "board:8889", "user@board", "board/path", "fe80::1%wlan0"
  ]) {
    const expression = `normalizeMediaHost(${JSON.stringify(invalid)})`;
    assert.equal(vm.runInContext(expression, validation.context), null, invalid);
  }

  const video = validation.elements.get("video");
  assert.deepEqual(video.assignedUrls, ["http://192.168.1.45:8889/cam01"]);
  assert.equal(validation.elements.get("state").textContent, "BACKOFF");
  assert.equal(validation.elements.get("pid").textContent, "unavailable");

  const transitions = await loadPage("?media_host=192.168.1.45", [
    metrics("BACKOFF"), metrics("RUNNING"), metrics("RUNNING"),
    metrics("BACKOFF"), metrics("RUNNING")
  ]);
  const transitionVideo = transitions.elements.get("video");
  assert.equal(transitionVideo.assignedUrls.length, 1);
  await transitions.refresh();
  assert.equal(transitionVideo.assignedUrls.length, 2,
               "BACKOFF to RUNNING must reload the player once");
  await transitions.refresh();
  assert.equal(transitionVideo.assignedUrls.length, 2,
               "steady RUNNING must not reload the player");
  await transitions.refresh();
  await transitions.refresh();
  assert.equal(transitionVideo.assignedUrls.length, 3,
               "a later recovery must reload the player once again");

  const transientError = await loadPage("?media_host=board.local", [
    metrics("RUNNING"), new Error("temporary API failure"), metrics("RUNNING")
  ]);
  await transientError.refresh();
  await transientError.refresh();
  assert.equal(transientError.elements.get("video").assignedUrls.length, 1,
               "an API error during RUNNING must not cause a needless reload");

  const ipv6 = await loadPage("?media_host=2001%3Adb8%3A%3A1", [metrics("RUNNING")]);
  assert.deepEqual(ipv6.elements.get("video").assignedUrls,
                   ["http://[2001:db8::1]:8889/cam01"]);

  const rejected = await loadPage("?media_host=user%40board", [metrics("FAILED")]);
  assert.equal(rejected.elements.get("video").assignedUrls.length, 0);
  assert.match(rejected.elements.get("hint").textContent, /media_host unavailable/);

  console.log("Diagnostic page behavior tests passed.");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});

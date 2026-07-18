import test from "node:test";
import assert from "node:assert/strict";

import { ensureDockerContainer } from "../scripts/devbox-guardian.mjs";

const environment = {
  DEVBOX_PROJECT_ROOT: "/repo",
  DEVBOX_IMAGE_NAME: "devbox:test",
  HOST_WORKSPACE_PATH: "/repo/workspace",
  DEVBOX_WORKSPACE_PATH: "/workspace",
};
const settings = { DevboxContainerName: "devbox-runtime" };
const result = (exitCode, stdout = "", stderr = "") => ({ exitCode, stdout, stderr });

test("stale container repair starts an existing stopped container without docker run", async () => {
  const calls = [];
  const responses = [result(0, "false\n"), result(0, "devbox-runtime\n"), result(0, "true\n")];
  const runner = async (_environment, args) => {
    calls.push(args);
    return responses.shift();
  };

  const repaired = await ensureDockerContainer(environment, settings, 5, runner);
  assert.equal(repaired.action, "started-existing");
  assert.deepEqual(calls.map((args) => args[0]), ["container", "start", "container"]);
  assert.equal(calls.some((args) => args[0] === "run"), false);
});

test("ambiguous Docker inspect errors never fall through to docker run", async () => {
  const calls = [];
  const runner = async (_environment, args) => {
    calls.push(args);
    return result(124, "", "Docker Desktop timed out");
  };

  await assert.rejects(
    ensureDockerContainer(environment, settings, 5, runner),
    /refusing a conflicting docker run/u,
  );
  assert.deepEqual(calls.map((args) => args[0]), ["container"]);
});

test("a stopped container that cannot start is removed and replaced", async () => {
  const calls = [];
  const responses = [
    result(0, "false\n"),
    result(1, "", "start failed"),
    result(0, "devbox-runtime\n"),
    result(0, "new-container-id\n"),
  ];
  const runner = async (_environment, args) => {
    calls.push(args);
    return responses.shift();
  };

  const repaired = await ensureDockerContainer(environment, settings, 5, runner);
  assert.equal(repaired.action, "created");
  assert.deepEqual(calls.map((args) => args[0]), ["container", "start", "rm", "run"]);
});

test("a create race re-inspects and starts the named container", async () => {
  const calls = [];
  const responses = [
    result(1, "", "Error: No such container: devbox-runtime"),
    result(125, "", "Conflict. The container name /devbox-runtime is already in use"),
    result(0, "false\n"),
    result(0, "devbox-runtime\n"),
  ];
  const runner = async (_environment, args) => {
    calls.push(args);
    return responses.shift();
  };

  const repaired = await ensureDockerContainer(environment, settings, 5, runner);
  assert.equal(repaired.action, "started-raced-existing");
  assert.deepEqual(calls.map((args) => args[0]), ["container", "run", "container", "start"]);
});

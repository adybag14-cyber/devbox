#!/usr/bin/env node

import { runLauncher } from "../src/launcher.js";

const result = await runLauncher(process.argv.slice(2));

if (result.command === "run") {
  console.log(`devbox running in foreground at ${result.url}`);
} else if (result.running) {
  if (result.stopRefused) {
    console.log(`devbox ${result.command} not performed`);
    process.exitCode = 2;
  } else {
    console.log(`devbox ${result.command} ok`);
  }
  console.log(`url: ${result.url}`);
  if (result.pid) {
    console.log(`pid: ${result.pid}`);
  }
  if (result.manager) {
    console.log(`manager: ${result.manager}`);
  }
  if (result.implementation) {
    console.log(`implementation: ${result.implementation}`);
  }
  console.log(`health: ${result.healthy === false ? "unhealthy" : "healthy"}`);
  console.log(`log: ${result.logFile}`);
  if (result.note) {
    console.log(`note: ${result.note}`);
  }
} else {
  console.log(`devbox ${result.command} ok`);
  console.log(`url: ${result.url}`);
  console.log(`log: ${result.logFile}`);
  console.log("status: stopped");
}

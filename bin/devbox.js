#!/usr/bin/env node

import { runLauncher } from "../src/launcher.js";

const result = await runLauncher(process.argv.slice(2));

if (result.command === "run") {
  console.log(`devbox running in foreground at ${result.url}`);
} else if (result.running) {
  console.log(`devbox ${result.command} ok`);
  console.log(`url: ${result.url}`);
  if (result.pid) {
    console.log(`pid: ${result.pid}`);
  }
  console.log(`log: ${result.logFile}`);
} else {
  console.log(`devbox ${result.command} ok`);
  console.log(`url: ${result.url}`);
  console.log(`log: ${result.logFile}`);
  console.log("status: stopped");
}

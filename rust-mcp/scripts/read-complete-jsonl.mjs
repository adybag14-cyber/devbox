import { readFile } from "node:fs/promises";
import { setTimeout as delay } from "node:timers/promises";

// A live append may end a read in the middle of a JSONL record. Only a newline
// commits that record; completed malformed records must still fail immediately.
export async function readCompleteJsonl(file, { timeoutMs = 2000, pollMs = 20 } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const text = await readFile(file, "utf8");
    const end = text.lastIndexOf("\n") + 1;
    const events = text.slice(0, end).split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
    if (text.length > 0 && end === text.length) return { text, events };
    if (Date.now() >= deadline) throw new Error(`Telemetry JSONL did not finish its final line: ${file}`);
    await delay(Math.min(pollMs, Math.max(1, deadline - Date.now())));
  }
}

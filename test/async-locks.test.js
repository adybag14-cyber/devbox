import test from "node:test";
import assert from "node:assert/strict";

import { AsyncReadWriteLock, KeyedReadWriteLock } from "../src/async-locks.js";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

test("AsyncReadWriteLock allows parallel readers", async () => {
  const lock = new AsyncReadWriteLock();
  let activeReaders = 0;
  let maxActiveReaders = 0;

  await Promise.all(
    Array.from({ length: 4 }, () =>
      lock.runRead(async () => {
        activeReaders += 1;
        maxActiveReaders = Math.max(maxActiveReaders, activeReaders);
        await sleep(25);
        activeReaders -= 1;
      }),
    ),
  );

  assert.equal(maxActiveReaders, 4);
});

test("AsyncReadWriteLock gives queued writers an exclusive boundary", async () => {
  const lock = new AsyncReadWriteLock();
  const events = [];
  let releaseFirstRead;

  const firstRead = lock.runRead(
    () =>
      new Promise((resolve) => {
        events.push("read-1-start");
        releaseFirstRead = () => {
          events.push("read-1-end");
          resolve();
        };
      }),
  );

  await sleep(10);

  const writer = lock.runWrite(async () => {
    events.push("write");
  });

  const secondRead = lock.runRead(async () => {
    events.push("read-2");
  });

  await sleep(10);
  assert.deepEqual(events, ["read-1-start"]);
  releaseFirstRead();
  await Promise.all([firstRead, writer, secondRead]);

  assert.deepEqual(events, ["read-1-start", "read-1-end", "write", "read-2"]);
});

test("KeyedReadWriteLock serializes same-key writers while allowing different keys", async () => {
  const lock = new KeyedReadWriteLock();
  const events = [];

  const sameKeyA = lock.runWrite("same", async () => {
    events.push("same-a-start");
    await sleep(30);
    events.push("same-a-end");
  });

  const sameKeyB = lock.runWrite("same", async () => {
    events.push("same-b");
  });

  const otherKey = lock.runWrite("other", async () => {
    events.push("other");
  });

  await Promise.all([sameKeyA, sameKeyB, otherKey]);

  assert.equal(events.indexOf("other") > -1, true);
  assert.deepEqual(
    events.filter((event) => event.startsWith("same")),
    ["same-a-start", "same-a-end", "same-b"],
  );
});

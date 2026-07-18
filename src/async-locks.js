export class AsyncMutex {
  #locked = false;
  #queue = [];

  acquire() {
    return new Promise((resolve) => {
      const grant = () => {
        this.#locked = true;
        let released = false;
        resolve(() => {
          if (released) {
            return;
          }

          released = true;
          this.#locked = false;
          this.#drain();
        });
      };

      this.#queue.push(grant);
      this.#drain();
    });
  }

  async runExclusive(callback) {
    const release = await this.acquire();
    try {
      return await callback();
    } finally {
      release();
    }
  }

  #drain() {
    if (this.#locked) {
      return;
    }

    const next = this.#queue.shift();
    if (next) {
      next();
    }
  }
}

export class AsyncReadWriteLock {
  #activeReaders = 0;
  #activeWriter = false;
  #queue = [];

  acquireRead() {
    return new Promise((resolve) => {
      this.#queue.push({ type: "read", resolve });
      this.#drain();
    });
  }

  acquireWrite() {
    return new Promise((resolve) => {
      this.#queue.push({ type: "write", resolve });
      this.#drain();
    });
  }

  async runRead(callback) {
    const release = await this.acquireRead();
    try {
      return await callback();
    } finally {
      release();
    }
  }

  async runWrite(callback) {
    const release = await this.acquireWrite();
    try {
      return await callback();
    } finally {
      release();
    }
  }

  #makeRelease(kind) {
    let released = false;
    return () => {
      if (released) {
        return;
      }

      released = true;
      if (kind === "read") {
        this.#activeReaders -= 1;
      } else {
        this.#activeWriter = false;
      }
      this.#drain();
    };
  }

  #drain() {
    if (this.#activeWriter || this.#queue.length === 0) {
      return;
    }

    const next = this.#queue[0];
    if (next.type === "write") {
      if (this.#activeReaders > 0) {
        return;
      }

      this.#queue.shift();
      this.#activeWriter = true;
      next.resolve(this.#makeRelease("write"));
      return;
    }

    while (this.#queue.length > 0 && this.#queue[0].type === "read" && !this.#activeWriter) {
      const read = this.#queue.shift();
      this.#activeReaders += 1;
      read.resolve(this.#makeRelease("read"));
    }
  }
}

export class KeyedReadWriteLock {
  #locks = new Map();

  async runRead(key, callback) {
    const entry = this.#entryFor(key);
    try {
      return await entry.lock.runRead(callback);
    } finally {
      this.#releaseEntry(key, entry);
    }
  }

  async runWrite(key, callback) {
    const entry = this.#entryFor(key);
    try {
      return await entry.lock.runWrite(callback);
    } finally {
      this.#releaseEntry(key, entry);
    }
  }

  #entryFor(key) {
    const normalizedKey = String(key);
    let entry = this.#locks.get(normalizedKey);
    if (!entry) {
      entry = {
        lock: new AsyncReadWriteLock(),
        refs: 0,
      };
      this.#locks.set(normalizedKey, entry);
    }

    entry.refs += 1;
    return entry;
  }

  #releaseEntry(key, entry) {
    entry.refs -= 1;
    if (entry.refs === 0 && this.#locks.get(String(key)) === entry) {
      this.#locks.delete(String(key));
    }
  }
}

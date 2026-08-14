export const refreshExecutionStoreHealth = async ({
  jobsRoot,
  probeWritablePath,
  probeExecutionSlotStoreWritable,
  statfs,
  minimumFreeBytes,
  warningFreeBytes = 50 * 1024 * 1024 * 1024,
  warningFreePercent = 5,
}) => {
  const startedAt = Date.now();
  const errors = [];
  let jobsWritable = false;
  let schedulerWritable = false;
  let freeBytes = 0;
  let totalBytes = 0;
  try { jobsWritable = await probeWritablePath(jobsRoot, "jobs"); } catch (error) { errors.push(`jobs: ${error.message}`); }
  try { schedulerWritable = await probeExecutionSlotStoreWritable(); } catch (error) { errors.push(`scheduler: ${error.message}`); }
  try {
    const fs = await statfs(jobsRoot);
    freeBytes = Number(fs.bavail) * Number(fs.bsize);
    totalBytes = Number(fs.blocks) * Number(fs.bsize);
    if (freeBytes < minimumFreeBytes) errors.push(`free disk ${freeBytes} below minimum ${minimumFreeBytes}`);
  } catch (error) {
    errors.push(`disk: ${error.message}`);
  }
  const freePercent = totalBytes > 0 ? (freeBytes * 100) / totalBytes : 0;
  const diskOk = errors.every((error) => !error.startsWith("disk:") && !error.startsWith("free disk"));
  const diskPressure = !diskOk || freeBytes < minimumFreeBytes
    ? "critical"
    : freeBytes < warningFreeBytes || freePercent < warningFreePercent ? "warning" : "normal";
  return {
    ok: errors.length === 0,
    sampledAtUtc: new Date().toISOString(),
    sampledAtMs: Date.now(),
    durationMs: Date.now() - startedAt,
    jobsWritable,
    schedulerWritable,
    freeBytes,
    totalBytes,
    freePercent,
    diskPressure,
    warningFreeBytes,
    warningFreePercent,
    minimumFreeBytes,
    error: errors.length > 0 ? errors.join("; ") : null,
  };
};
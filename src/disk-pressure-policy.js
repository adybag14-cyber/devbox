const CLEANUP_MARKERS = Object.freeze([
  "rm -", "remove-item", "rmdir", " del ", "git clean", "cargo clean",
  "npm cache clean", "npm cache verify", "pip cache purge", "docker system prune",
  "docker builder prune", "docker image prune", "docker container prune", "docker volume prune",
  "wsl --shutdown",
]);

export const diskPressureCleanupOperation = (operation) => {
  const text = String(operation ?? "").toLowerCase();
  return CLEANUP_MARKERS.some((marker) => text.includes(marker));
};

export const shouldRejectDiskPressure = ({
  diskPressure,
  resourceClass,
  readOnly = false,
  operation = "",
} = {}) =>
  diskPressure === "critical" &&
  !readOnly &&
  ["heavy", "io-heavy"].includes(String(resourceClass ?? "")) &&
  !diskPressureCleanupOperation(operation);

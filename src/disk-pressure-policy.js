const CLEANUP_PATTERNS = Object.freeze([
  /^rm\s+-/u,
  /^remove-item(?:\s|$)/u,
  /^rmdir(?:\s|$)/u,
  /^del(?:\s|$)/u,
  /^git\s+clean(?:\s|$)/u,
  /^cargo\s+clean(?:\s|$)/u,
  /^npm\s+cache\s+(?:clean|verify)(?:\s|$)/u,
  /^pip\s+cache\s+purge(?:\s|$)/u,
  /^docker\s+(?:system|builder|image|container|volume)\s+prune(?:\s|$)/u,
  /^wsl\s+--shutdown(?:\s|$)/u,
]);

const COMMAND_SEPARATOR_RE = /[;&|\r\n]/u;

export const diskPressureCleanupOperation = (operation) => {
  const text = String(operation ?? "").trim().toLowerCase().replace(/\s+/gu, " ");
  if (!text || COMMAND_SEPARATOR_RE.test(text)) return false;
  return CLEANUP_PATTERNS.some((pattern) => pattern.test(text));
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

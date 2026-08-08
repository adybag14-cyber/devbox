const marker = (omittedChars, mode) => `\n... ${mode} output omitted ${Math.max(0, omittedChars)} characters ...\n`;

const byChars = (text, maxChars, mode) => {
  const value = String(text ?? "");
  const limit = Math.max(100, Number(maxChars) || 100);
  if (value.length <= limit) return { text: value, truncated: false, originalChars: value.length };
  if (mode === "head") {
    const note = marker(value.length - limit, "tail");
    return { text: value.slice(0, Math.max(0, limit - note.length)) + note, truncated: true, originalChars: value.length };
  }
  if (mode === "summary") {
    const note = marker(value.length - limit, "middle");
    const available = Math.max(0, limit - note.length);
    const head = Math.floor(available / 2);
    const tail = available - head;
    return { text: value.slice(0, head) + note + value.slice(Math.max(0, value.length - tail)), truncated: true, originalChars: value.length };
  }
  const note = marker(value.length - limit, "head");
  return { text: note + value.slice(Math.max(0, value.length - (limit - note.length))), truncated: true, originalChars: value.length };
};

const byLines = (text, maxLines, mode) => {
  const value = String(text ?? "");
  const limit = Math.max(0, Number(maxLines) || 0);
  if (limit <= 0) return { text: value, truncated: false, originalLines: null };
  const trailingNewline = /\r?\n$/u.test(value);
  const content = trailingNewline ? value.replace(/\r?\n$/u, "") : value;
  const lines = content.split(/\r?\n/u);
  const originalLines = lines.length;
  if (originalLines <= limit) return { text: value, truncated: false, originalLines };
  const suffix = trailingNewline ? "\n" : "";
  if (mode === "head") {
    return {
      text: `${lines.slice(0, limit).join("\n")}\n... tail lines omitted ...${suffix}`,
      truncated: true,
      originalLines,
    };
  }
  if (mode === "summary") {
    const head = Math.floor(limit / 2);
    const tail = limit - head;
    return {
      text: `${lines.slice(0, head).join("\n")}\n... middle lines omitted ...\n${lines.slice(lines.length - tail).join("\n")}${suffix}`,
      truncated: true,
      originalLines,
    };
  }
  return {
    text: `... head lines omitted ...\n${lines.slice(lines.length - limit).join("\n")}${suffix}`,
    truncated: true,
    originalLines,
  };
};

export const shapeProcessOutput = (text, {
  mode = "tail",
  maxChars = 65536,
  maxLines = 0,
} = {}) => {
  const normalizedMode = ["head", "tail", "summary"].includes(String(mode).toLowerCase())
    ? String(mode).toLowerCase()
    : "tail";
  const originalText = String(text ?? "");
  const lineResult = byLines(originalText, maxLines, normalizedMode);
  const charResult = byChars(lineResult.text, maxChars, normalizedMode);
  return {
    ...charResult,
    originalChars: originalText.length,
    truncated: lineResult.truncated || charResult.truncated,
    originalLines: lineResult.originalLines,
    mode: normalizedMode,
  };
};

export const outputShapingInternals = { byChars, byLines };

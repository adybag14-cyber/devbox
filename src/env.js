import { readFile } from "node:fs/promises";

const validKey = /^[A-Za-z_][A-Za-z0-9_]*$/;

const decodeDoubleQuoted = (value) =>
  value.replace(/\\(n|r|t|\\|")/g, (_match, escaped) => {
    if (escaped === "n") return "\n";
    if (escaped === "r") return "\r";
    if (escaped === "t") return "\t";
    return escaped;
  });

const parseValue = (rawValue) => {
  const value = String(rawValue ?? "").trim();
  if (!value) return "";

  if (value.startsWith('"') && value.endsWith('"')) {
    return decodeDoubleQuoted(value.slice(1, -1));
  }

  if (value.startsWith("'") && value.endsWith("'")) {
    return value.slice(1, -1);
  }

  const commentIndex = value.search(/\s+#/);
  return (commentIndex >= 0 ? value.slice(0, commentIndex) : value).trim();
};

export const parseEnvText = (content) => {
  const parsed = {};
  const lines = String(content ?? "").replace(/^\uFEFF/, "").split(/\r?\n/);

  for (const originalLine of lines) {
    let line = originalLine.trim();
    if (!line || line.startsWith("#")) continue;
    if (line.startsWith("export ")) line = line.slice(7).trimStart();

    const separator = line.indexOf("=");
    if (separator < 1) continue;

    const key = line.slice(0, separator).trim();
    if (!validKey.test(key)) continue;
    parsed[key] = parseValue(line.slice(separator + 1));
  }

  return parsed;
};

export const loadEnvFile = async (filePath, env = process.env) => {
  let content;
  try {
    content = await readFile(filePath, "utf8");
  } catch (error) {
    if (error?.code === "ENOENT") return { loaded: false, filePath, keys: [] };
    throw error;
  }

  const parsed = parseEnvText(content);
  const loadedKeys = [];
  for (const [key, value] of Object.entries(parsed)) {
    if (Object.prototype.hasOwnProperty.call(env, key)) continue;
    env[key] = value;
    loadedKeys.push(key);
  }

  return { loaded: true, filePath, keys: loadedKeys };
};

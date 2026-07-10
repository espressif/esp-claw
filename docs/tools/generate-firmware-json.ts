#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import { promises as fs } from "node:fs";
import path from "node:path";
import { execSync } from "node:child_process";
import { fileURLToPath } from "node:url";

interface PartitionEntry {
  name: string;
  type: string;
  subtype: string;
  offset: string;
  size: string;
}

interface FlashSettings {
  flash_mode: string;
  flash_freq: string;
  flash_size: string;
}

interface PackageMeta {
  refs: string;
  application: string;
  chip: string;
  brand: string;
  board_name: string;
  console_output: string;
  commit_timestamp: string;
  partition_table: PartitionEntry[];
  flash_files: Record<string, string>;
  flash_settings: FlashSettings;
  min_flash_size: string;
  nvs_info?: { start_addr: string; size: string };
}

interface FirmwareConsoleEntry {
  flash_files: Record<string, string>;
  flash_settings: FlashSettings;
  partition_table: PartitionEntry[];
  nvs_info?: { start_addr: string; size: string };
  min_flash_size: number;
  min_psram_size: number;
}

type FirmwareBoard = Record<string, FirmwareConsoleEntry>;
type FirmwareBrand = Record<string, FirmwareBoard>;
type FirmwareChip = Record<string, FirmwareBrand>;
type FirmwareDb = Record<string, FirmwareChip>;

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOCS_ROOT = path.resolve(SCRIPT_DIR, "..");
const TARGET_FIRMWARE_DIR = path.join(DOCS_ROOT, "public", "firmware", "master");
const TARGET_FIRMWARE_JSON = path.join(DOCS_ROOT, "src", "flash-tool", "firmware.json");
const TARGET_FIRMWARE_META_JSON = path.join(DOCS_ROOT, "src", "flash-tool", "firmware-meta.json");

function log(msg: string): void {
  console.error(msg);
}

function getMasterRef(): string {
  try {
    const desc = execSync("git describe --tags --long --always", { stdio: ["pipe", "pipe", "pipe"] })
      .toString()
      .trim();
    if (desc) return desc;
  } catch {}
  try {
    const hash = execSync("git rev-parse --short HEAD", { stdio: ["pipe", "pipe", "pipe"] })
      .toString()
      .trim();
    if (hash) return hash;
  } catch {}
  return new Date().toISOString().slice(0, 19).replace("T", "_").replace(/:/g, "");
}

function parseFlashMB(value: unknown): number {
  if (typeof value === "number" && Number.isInteger(value)) {
    return value;
  }

  if (typeof value !== "string") {
    throw new Error(`unsupported min_flash_size type: ${typeof value}`);
  }

  const text = value.trim();
  if (!text) {
    throw new Error("empty min_flash_size");
  }

  const upper = text.toUpperCase();
  if (upper.endsWith("MB")) {
    return Number.parseInt(upper.slice(0, -2).trim(), 0);
  }
  if (upper.endsWith("M")) {
    return Number.parseInt(upper.slice(0, -1).trim(), 0);
  }

  const num = Number.parseInt(text, 0);
  if (num % (1024 * 1024) === 0) {
    return num / (1024 * 1024);
  }

  return num;
}

function parseRevision(chip: string, sdkconfigPath: string | null): number | null {
  // Revision handling is chip-specific; for now only esp32p4 uses it
  // In per-partition mode, revision info comes from the build metadata
  return null;
}

function makeChipKey(chip: string): string {
  return chip;
}

async function loadMetadataFiles(firmwareOutputDir: string): Promise<PackageMeta[]> {
  const dirEntries = await fs.readdir(firmwareOutputDir, { withFileTypes: true });
  const metadataFiles = dirEntries
    .filter((entry) => entry.isFile() && entry.name.endsWith(".json"))
    .map((entry) => entry.name)
    .sort((a, b) => a.localeCompare(b));

  if (metadataFiles.length === 0) {
    throw new Error(`No metadata json found in ${firmwareOutputDir}`);
  }

  const records: PackageMeta[] = [];
  for (const fileName of metadataFiles) {
    const filePath = path.join(firmwareOutputDir, fileName);
    const raw = await fs.readFile(filePath, "utf8");
    const data = JSON.parse(raw);
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      throw new Error(`Invalid metadata format: ${filePath}`);
    }
    records.push(data as PackageMeta);
  }

  return records;
}

async function extractTarGz(tarGzPath: string, destDir: string): Promise<void> {
  await fs.mkdir(destDir, { recursive: true });
  execSync(`tar xzf "${tarGzPath}" -C "${destDir}"`, { stdio: "pipe" });
}

function usage(): string {
  return [
    "Usage:",
    "  node ./docs/tools/generate-firmware-json.ts <firmware_output_dir> [--allow-empty]",
    "",
    "Example:",
    "  node ./docs/tools/generate-firmware-json.ts ./firmware_output",
    "  node ./docs/tools/generate-firmware-json.ts ./firmware_output --allow-empty",
  ].join("\n");
}

async function main(): Promise<number> {
  const argv = process.argv.slice(2);
  const allowEmpty = argv.includes("--allow-empty");
  const positional = argv.filter((arg) => !arg.startsWith("--"));
  const sourceArg = positional[0]?.trim();
  if (!sourceArg) {
    log("Missing <firmware_output_dir> argument.");
    log(usage());
    return 1;
  }

  const firmwareOutputDir = path.resolve(process.cwd(), sourceArg);
  let stat;
  try {
    stat = await fs.stat(firmwareOutputDir);
  } catch {
    if (allowEmpty) {
      log(`warning: firmware_output directory not found, skip generation: ${firmwareOutputDir}`);
      return 0;
    }

    log(`firmware_output directory not found: ${firmwareOutputDir}`);
    return 1;
  }
  if (!stat.isDirectory()) {
    log(`firmware_output path is not a directory: ${firmwareOutputDir}`);
    return 1;
  }

  let records: PackageMeta[];
  try {
    records = await loadMetadataFiles(firmwareOutputDir);
  } catch (error) {
    if (allowEmpty) {
      log(`warning: ${(error as Error).message}, skip generation`);
      return 0;
    }
    log((error as Error).message);
    return 1;
  }

  await fs.rm(TARGET_FIRMWARE_DIR, { recursive: true, force: true });
  await fs.mkdir(TARGET_FIRMWARE_DIR, { recursive: true });

  // Group by application → chip → brand → board → console_output
  const firmware: Record<string, FirmwareDb> = {};

  for (const meta of records) {
    const { application, chip, brand, board_name, console_output, flash_files, flash_settings, partition_table, nvs_info } = meta;

    if (!chip || !board_name || !brand || !console_output) {
      log(`skip one metadata: missing required fields (${JSON.stringify(meta)})`);
      continue;
    }

    const appKey = (application || "default").trim();
    const tarGzName = `${board_name}__${console_output.replace(/ /g, "_").toLowerCase()}.tar.gz`;
    const tarGzPath = path.join(firmwareOutputDir, tarGzName);

    const extractDir = path.join(TARGET_FIRMWARE_DIR, board_name, console_output);

    if (await fs.stat(tarGzPath).catch(() => null)) {
      try {
        await extractTarGz(tarGzPath, extractDir);
      } catch (e) {
        log(`skip: tar extraction failed for ${tarGzName}: ${e}`);
        continue;
      }
    } else {
      log(`warning: tar.gz not found for ${board_name}/${console_output}, metadata-only entry`);
      await fs.mkdir(extractDir, { recursive: true });
    }

    const basePath = `/firmware/master/${board_name}/${console_output}`;
    const resolvedFlashFiles: Record<string, string> = {};
    for (const [offset, filename] of Object.entries(flash_files)) {
      resolvedFlashFiles[offset] = `${basePath}/${filename}`;
    }

    let minFlashMB: number;
    try {
      minFlashMB = parseFlashMB(meta.min_flash_size);
    } catch (error) {
      log(`skip one metadata: invalid min_flash_size (${(error as Error).message})`);
      continue;
    }

    const chipKey = makeChipKey(chip.trim());
    const brandKey = brand.trim();
    const boardKey = board_name.trim();

    if (!firmware[appKey]) {
      firmware[appKey] = {};
    }
    if (!firmware[appKey][chipKey]) {
      firmware[appKey][chipKey] = {};
    }
    if (!firmware[appKey][chipKey][brandKey]) {
      firmware[appKey][chipKey][brandKey] = {};
    }
    if (!firmware[appKey][chipKey][brandKey][boardKey]) {
      firmware[appKey][chipKey][brandKey][boardKey] = {};
    }

    firmware[appKey][chipKey][brandKey][boardKey][console_output] = {
      flash_files: resolvedFlashFiles,
      flash_settings: flash_settings,
      partition_table: partition_table,
      nvs_info: nvs_info,
      min_flash_size: minFlashMB,
      min_psram_size: 0,
    };
  }

  if (Object.keys(firmware).length === 0) {
    if (allowEmpty) {
      log("warning: No valid metadata collected, writing empty firmware.json");
      await fs.writeFile(TARGET_FIRMWARE_JSON, "{}\n", "utf8");
      return 0;
    }
    log("No valid metadata collected from firmware_output/*.json");
    return 1;
  }

  const sortedFirmware = sortDeep(firmware);

  await fs.writeFile(TARGET_FIRMWARE_JSON, `${JSON.stringify(sortedFirmware, null, 2)}\n`, "utf8");
  console.log(`Extracted firmware files to: ${TARGET_FIRMWARE_DIR}`);
  console.log(`Generated: ${TARGET_FIRMWARE_JSON}`);

  const masterRef = getMasterRef();
  await fs.writeFile(
    TARGET_FIRMWARE_META_JSON,
    `${JSON.stringify({ masterRef }, null, 2)}\n`,
    "utf8",
  );
  console.log(`Generated: ${TARGET_FIRMWARE_META_JSON} (masterRef=${masterRef})`);

  return 0;
}

function sortDeep(obj: Record<string, unknown>): Record<string, unknown> {
  const sorted: Record<string, unknown> = {};
  for (const key of Object.keys(obj).sort()) {
    const val = obj[key];
    if (val && typeof val === "object" && !Array.isArray(val)) {
      sorted[key] = sortDeep(val as Record<string, unknown>);
    } else {
      sorted[key] = val;
    }
  }
  return sorted;
}

main()
  .then((code) => {
    process.exitCode = code;
  })
  .catch((error) => {
    log((error as Error).stack ?? String(error));
    process.exitCode = 1;
  });

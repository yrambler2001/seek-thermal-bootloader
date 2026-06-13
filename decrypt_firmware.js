#!/usr/bin/env node
/*
 * decrypt_firmware.js
 * ------------------------------------------------------------------------
 * Extract and decrypt the application firmware image(s) from a full flash
 * dump of an NXP LPC43xx device that uses the "80K_4320_Bootloader".
 *
 * Everything needed is taken FROM THE DUMP ITSELF:
 *   - the two 128-bit keys live in the bootloader region
 *       Key A @ file 0x2F70   (flash 0x14002F70)
 *       Key B @ file 0x2F80   (flash 0x14002F80)
 *   - an optional per-device key override slot @ file 0x218 (flash 0x14000218);
 *     if it is neither all-00 nor all-FF it replaces Key B (matches the loader).
 *   - firmware images sit at 64 KiB-aligned slots; each image carries a
 *     cleartext 64-byte header at image+0x200 (magic 0xA1B2C3D4 + length).
 *
 * Cipher (reverse-engineered from the bootloader):
 *   - Marsaglia xorshift128, shifts 11/8/19, state = [x,y,z,w].
 *   - Key -> seed: x=k1^K, y=k2^K, z=k3^K, w=k0^K   (K = 0x13579BDF,
 *     k0..k3 = the key's four little-endian 32-bit words).
 *   - The image is processed one 32-bit word at a time. One keystream word is
 *     generated per image word and the generator ALWAYS advances. Word indices
 *     128..143 (byte offset 0x200..0x23F, the header) are left as cleartext;
 *     every other word is XORed with the keystream word.
 *   - Acceptance test: the 16-bit sum of all (decrypted) words must equal
 *     0xFFFF. The loader tries Key B first, then Key A.
 *
 * Usage:
 *   node decrypt_firmware.js <full_dump.bin> [output_dir]
 *
 * Output: one decrypted .bin per distinct encrypted image found, named
 *   decrypted_<flashAddr>_key<A|B>.bin, plus a short report on stdout.
 * ------------------------------------------------------------------------
 */

'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// ----- layout constants (from the reverse engineering) -------------------
const FLASH_BASE   = 0x14000000;        // SPIFI memory-mapped base
const OFF_KEY_A    = 0x2F70;            // file offset of Key A (16 bytes)
const OFF_KEY_B    = 0x2F80;            // file offset of Key B (16 bytes)
const OFF_DEV_SLOT = 0x218;             // file offset of per-device key slot
const OFF_BOOTCFG  = 0x10000;           // file offset of boot-config record
const HDR_OFF      = 0x200;             // header window inside an image
const MAGIC        = 0xA1B2C3D4;        // image header magic
const MAX_IMG_LEN  = 0x1C000;           // loader's monolithic size gate
const SLOT_STEP    = 0x10000;           // images are 64 KiB-aligned
const SEED_K       = 0x13579BDF;        // key-whitening constant
const HDR_LO = 128, HDR_HI = 143;       // word indices kept as cleartext

// ----- xorshift128 keystream ---------------------------------------------
function seedFromKey(buf, off) {
  const k0 = buf.readUInt32LE(off + 0);
  const k1 = buf.readUInt32LE(off + 4);
  const k2 = buf.readUInt32LE(off + 8);
  const k3 = buf.readUInt32LE(off + 12);
  // state = [x, y, z, w]
  return Uint32Array.of(
    (k1 ^ SEED_K) >>> 0,
    (k2 ^ SEED_K) >>> 0,
    (k3 ^ SEED_K) >>> 0,
    (k0 ^ SEED_K) >>> 0
  );
}

function xsNext(s) {
  const x = s[0];
  const v1 = (x ^ (x << 11)) >>> 0;
  s[0] = s[1];
  s[1] = s[2];
  const w = s[3];
  s[2] = w;
  const v3 = (w ^ (w >>> 19) ^ v1 ^ (v1 >>> 8)) >>> 0;
  s[3] = v3;
  return v3 >>> 0;
}

// True for every word index that must be XORed (i.e. NOT the header window).
function isEncryptedWord(i) {
  // mirrors the firmware's unsigned test ((i - 128) > 0xF)
  return ((i - HDR_LO) >>> 0) > (HDR_HI - HDR_LO);
}

// 16-bit acceptance checksum: sum of decrypted words must equal 0xFFFF.
function checksum16(src, byteLen, state) {
  let acc = 0;
  const n = byteLen >>> 2;
  for (let i = 0; i < n; i++) {
    const k = xsNext(state);
    const w = src.readUInt32LE(4 * i);
    const v = isEncryptedWord(i) ? (k ^ w) >>> 0 : w;
    acc = (acc + v) >>> 0;
  }
  return acc >>> 0;
}

// Decrypt the whole image into a new Buffer.
function decryptImage(src, byteLen, state) {
  const out = Buffer.allocUnsafe(byteLen);
  const n = byteLen >>> 2;
  for (let i = 0; i < n; i++) {
    const k = xsNext(state);
    const w = src.readUInt32LE(4 * i);
    const v = isEncryptedWord(i) ? (k ^ w) >>> 0 : w;
    out.writeUInt32LE(v >>> 0, 4 * i);
  }
  return out;
}

// ----- helpers ------------------------------------------------------------
const hx = (n, w = 8) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(w, '0');
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');
function allByte(buf, off, len, val) {
  for (let i = 0; i < len; i++) if (buf[off + i] !== val) return false;
  return true;
}

// ----- main ---------------------------------------------------------------
function main() {
  const dumpPath = process.argv[2];
  const outDir   = process.argv[3] || path.join(process.cwd(), 'decrypted');
  if (!dumpPath) {
    console.error('Usage: node decrypt_firmware.js <full_dump.bin> [output_dir]');
    process.exit(1);
  }
  const d = fs.readFileSync(dumpPath);
  fs.mkdirSync(outDir, { recursive: true });

  console.log(`Dump: ${dumpPath}  (${d.length} bytes, ${hx(d.length)})`);

  // 1) Pull the keys straight out of the dump.
  const keyA = d.subarray(OFF_KEY_A, OFF_KEY_A + 16);
  let   keyBoff = OFF_KEY_B;                 // default: hard-coded Key B
  let   keyBsrc = 'hard-coded Key B @0x14002F80';
  const slotBlank = allByte(d, OFF_DEV_SLOT, 16, 0x00) || allByte(d, OFF_DEV_SLOT, 16, 0xFF);
  if (!slotBlank) { keyBoff = OFF_DEV_SLOT; keyBsrc = 'per-device slot @0x14000218'; }

  console.log(`Key A : ${keyA.toString('hex')}`);
  console.log(`Key B : ${d.subarray(keyBoff, keyBoff + 16).toString('hex')}  (${keyBsrc})`);

  // Keyed seed factories (fresh state each call; the generator is consumed).
  const KEYS = {
    A: () => seedFromKey(d, OFF_KEY_A),
    B: () => seedFromKey(d, keyBoff),
  };

  // 2) Informational: list the boot-config slot table if present.
  if (d.length >= OFF_BOOTCFG + 16) {
    const cfg = [];
    for (let i = 4; i <= 12; i += 4) cfg.push(hx(d.readUInt32LE(OFF_BOOTCFG + i)));
    console.log(`Boot-config slot pointers @0x14010000: ${cfg.join(', ')} ` +
                `(active index ${d.readUInt32LE(OFF_BOOTCFG)})`);
  }

  // 3) Find candidate images at 64 KiB-aligned slots.
  const candidates = [];
  for (let base = 0; base + HDR_OFF + 8 <= d.length; base += SLOT_STEP) {
    if (d.readUInt32LE(base + HDR_OFF) !== (MAGIC >>> 0)) continue;
    const len = d.readUInt32LE(base + HDR_OFF + 4);
    if ((len & 3) !== 0 || len <= HDR_OFF || len >= MAX_IMG_LEN) continue;
    if (base + len > d.length) continue;
    candidates.push({ base, len });
  }

  if (candidates.length === 0) {
    console.error('No firmware images found (no cleartext header magic at any slot).');
    process.exit(2);
  }

  // 4) Validate + decrypt each candidate; remember which key works.
  const byCipher = new Map();   // ciphertext-hash -> first instance (de-dupes copies)
  const results = [];
  for (const c of candidates) {
    const src = d.subarray(c.base, c.base + c.len);
    let picked = null;
    for (const name of ['B', 'A']) {          // loader order: Key B first
      if (checksum16(src, c.len, KEYS[name]()) === 0xFFFF) { picked = name; break; }
    }
    const cipherHash = sha(src);
    const rec = {
      flash: FLASH_BASE + c.base, fileOff: c.base, len: c.len,
      key: picked, cipherHash,
      duplicateOf: byCipher.has(cipherHash) ? byCipher.get(cipherHash).flash : null,
    };
    if (!byCipher.has(cipherHash)) byCipher.set(cipherHash, rec);
    if (picked) {
      const plain = decryptImage(src, c.len, KEYS[picked]());
      rec.plainHash = sha(plain);
      rec.sp = plain.readUInt32LE(0);
      rec.entry = plain.readUInt32LE(4);
      rec.plain = plain;
    }
    results.push(rec);
  }

  // 5) Report + write one file per distinct ENCRYPTED image that validated.
  console.log(`\nFound ${results.length} image instance(s); ` +
              `${byCipher.size} distinct encrypted image(s).\n`);

  const written = [];
  const seenPlain = new Map();
  for (const r of results) {
    const tag = `@${hx(r.flash)} (file ${hx(r.fileOff)}), len ${hx(r.len)}`;
    if (!r.key) { console.log(`  ${tag}: header present but NO key validated -> skipped`); continue; }
    if (r.duplicateOf !== null) {
      console.log(`  ${tag}: Key ${r.key}, identical ciphertext to ${hx(r.duplicateOf)} -> copy, not re-written`);
      continue;
    }
    const name = `decrypted_${hx(r.flash, 8).slice(2)}_key${r.key}.bin`;
    const dest = path.join(outDir, name);
    fs.writeFileSync(dest, r.plain);
    written.push(dest);
    const samePlain = seenPlain.has(r.plainHash) ? ` (same plaintext as ${hx(seenPlain.get(r.plainHash))})`
                                                 : '';
    if (!seenPlain.has(r.plainHash)) seenPlain.set(r.plainHash, r.flash);
    console.log(`  ${tag}: Key ${r.key}  ->  ${name}`);
    console.log(`        SP=${hx(r.sp)}  entry=${hx(r.entry)}  sha256=${r.plainHash.slice(0, 32)}${samePlain}`);
  }

  console.log(`\nWrote ${written.length} decrypted firmware file(s) to ${outDir}`);
}

main();

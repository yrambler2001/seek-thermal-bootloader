#!/usr/bin/env node
/*
 * decrypt_firmware.js
 * ------------------------------------------------------------------------
 * Locate the obfuscation key(s) in a LPC43xx flash dump WITHOUT knowing
 * where the key is stored, then decrypt every application image slot found.
 * Works across the bootloader generations seen so far by trying each known
 * "cipher profile" (see PROFILES below).
 *
 * The cipher (reverse-engineered from several bootloaders):
 *   - Marsaglia xorshift128, shifts 11/8/19, state = [x,y,z,w].
 *   - Key (16 bytes = k0..k3 little-endian words) -> state:
 *         x = k1 ^ K,  y = k2 ^ K,  z = k3 ^ K,  w = k0 ^ K
 *     where K is a per-build whitening constant.
 *   - One keystream word per image word; the generator ALWAYS advances. Word
 *     indices 128..143 (bytes 0x200..0x23F, the cleartext header) are NOT
 *     XORed; every other word is XORed with the keystream word.
 *   - Acceptance: the 32-bit sum of all (decrypted) words equals TARGET.
 *
 * Known profiles:
 *   Build(s)                                                     K           TARGET
 *   -------------------------------------------------------------------------------
 *   2018.07.05_17.35.20 / 2019.01.07_15.56.20 /
 *   2021.08.12_08.20.23 builds                                  0x13579BDF  0x0000FFFF
 *
 *   2016.06.26_11.08.15 / 2016.06.26_11.08.46 builds            0x00000000  0x00000000
 *     Key stored un-whitened.
 *
 *   The 2016 default-key path keeps the key in flash .data verbatim, so a
 *   straight dump scan still finds it. (If a unit used the OTP-XOR key path
 *   instead, the seed depends on the chip's One-Time-Programmable memory,
 *   which is NOT in a flash dump and cannot be recovered from it.)
 *
 * Why this finds the key without an offset: the cipher is self-validating, so
 * we treat every 16-byte window as a candidate key and keep whatever makes the
 * checksum hit TARGET. A pre-filter on the decrypted reset vector (SP must be
 * SRAM-resident + word-aligned, entry must be a Thumb code address) discards
 * ~99.9999% of windows before the full checksum, so each scan is ~1-2 s.
 *
 * Output (per image slot), written next to the input dump by default:
 *   <dumpname>_decrypted_<FLASHADDR>.bin   and  .txt  (decryption info)
 *
 * Usage:
 *   node decrypt_firmware.js <full_dump.bin> [out_dir] [--align=1] [--no-write]
 * ------------------------------------------------------------------------
 */
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

// ----- cipher profiles ---------------------------------------------------
const PROFILES = [
  { name: 'xs128/K=13579BDF/sum=FFFF', K: 0x13579BDF, TARGET: 0x0000FFFF },
  { name: 'xs128/K=0/sum=0',          K: 0x00000000, TARGET: 0x00000000 },
];

// ----- cipher core -------------------------------------------------------
const HDR_LO = 128, HDR_HI = 143;
const MAGIC = 0xA1B2C3D4, HDR_OFF = 0x200, SLOT_STEP = 0x10000, MAX_IMG_LEN = 0x1C000;
const FLASH_BASE = 0x14000000;

function seedFromKeyBytes(b16, K) {
  const k0 = b16.readUInt32LE(0), k1 = b16.readUInt32LE(4),
        k2 = b16.readUInt32LE(8), k3 = b16.readUInt32LE(12);
  return Uint32Array.of((k1 ^ K) >>> 0, (k2 ^ K) >>> 0, (k3 ^ K) >>> 0, (k0 ^ K) >>> 0);
}
function xsNext(s) {
  const x = s[0], v1 = (x ^ (x << 11)) >>> 0;
  s[0] = s[1]; s[1] = s[2];
  const w = s[3]; s[2] = w;
  const v3 = (w ^ (w >>> 19) ^ v1 ^ (v1 >>> 8)) >>> 0;
  s[3] = v3; return v3 >>> 0;
}
const isEnc = (i) => (((i - HDR_LO) >>> 0) > (HDR_HI - HDR_LO));

function checksum32(src, byteLen, state) {       // 32-bit sum of decrypted words
  let acc = 0; const n = byteLen >>> 2;
  for (let i = 0; i < n; i++) {
    const k = xsNext(state), w = src.readUInt32LE(4 * i);
    acc = (acc + (isEnc(i) ? (k ^ w) >>> 0 : w)) >>> 0;
  }
  return acc >>> 0;
}
function decryptImage(src, byteLen, state) {
  const out = Buffer.allocUnsafe(byteLen); const n = byteLen >>> 2;
  for (let i = 0; i < n; i++) {
    const k = xsNext(state), w = src.readUInt32LE(4 * i);
    out.writeUInt32LE((isEnc(i) ? (k ^ w) >>> 0 : w) >>> 0, 4 * i);
  }
  return out;
}
const validKey = (b16, p, src, len) => checksum32(src, len, seedFromKeyBytes(b16, p.K)) === p.TARGET;

// ----- key discovery (scan whole dump for one profile) -------------------
function findKeyOffsets(dump, src, byteLen, ramPrefix, align, p) {
  const c0 = src.readUInt32LE(0), c1 = src.readUInt32LE(4), K = p.K;
  const hits = []; const end = dump.length - 16;
  for (let o = 0; o <= end; o += align) {
    const k0 = (dump.readUInt32LE(o + 4) ^ K) >>> 0;   // x
    const k1 = (dump.readUInt32LE(o + 8) ^ K) >>> 0;   // y
    const k3 = (dump.readUInt32LE(o + 0) ^ K) >>> 0;   // w
    let x = k0, w = k3, t = (x ^ (x << 11)) >>> 0;
    const o0 = (w ^ (w >>> 19) ^ t ^ (t >>> 8)) >>> 0;      // keystream[0]
    const sp = (o0 ^ c0) >>> 0;
    if ((sp >>> 24) !== ramPrefix || (sp & 3) !== 0) continue;
    x = k1; w = o0; t = (x ^ (x << 11)) >>> 0;
    const o1 = (w ^ (w >>> 19) ^ t ^ (t >>> 8)) >>> 0;      // keystream[1]
    const entry = (o1 ^ c1) >>> 0, et = entry >>> 24;
    if ((et !== 0x10 && et !== 0x14) || (entry & 1) !== 1) continue;
    if (checksum32(src, byteLen, seedFromKeyBytes(dump.subarray(o, o + 16), K)) === p.TARGET)
      hits.push({ off: o, sp, entry });
  }
  return hits;
}

// ----- helpers -----------------------------------------------------------
const hx = (n, w = 8) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(w, '0');
const addrTag = (n) => (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

function findImages(d) {
  const out = [];
  for (let b = 0; b + HDR_OFF + 8 <= d.length; b += SLOT_STEP) {
    if (d.readUInt32LE(b + HDR_OFF) !== (MAGIC >>> 0)) continue;
    const len = d.readUInt32LE(b + HDR_OFF + 4);
    if ((len & 3) !== 0 || len <= HDR_OFF || len >= MAX_IMG_LEN || b + len > d.length) continue;
    out.push({ base: b, len });
  }
  return out;
}

// ----- main --------------------------------------------------------------
function main() {
  const args = process.argv.slice(2);
  const flags = new Set(args.filter(a => a.startsWith('--')));
  const pos = args.filter(a => !a.startsWith('--'));
  const dumpPath = pos[0];
  if (!dumpPath) { console.error('Usage: node decrypt_firmware.js <full_dump.bin> [out_dir] [--align=1] [--no-write]'); process.exit(1); }
  const write = !flags.has('--no-write');
  const align = Math.max(1, parseInt((args.find(a => a.startsWith('--align=')) || '--align=1').split('=')[1], 10) || 1);

  const absDump = path.resolve(dumpPath);
  const stem = path.basename(absDump).replace(/\.[^.\/\\]+$/, '');
  const outDir = pos[1] ? path.resolve(pos[1]) : path.join(path.dirname(absDump), `${stem}_decrypted`);

  const d = fs.readFileSync(absDump);
  const ramPrefix = d.readUInt32LE(0) >>> 24;
  console.log(`Dump: ${absDump}  (${d.length} bytes)`);
  console.log(`Reset SP = ${hx(d.readUInt32LE(0))}  ->  SRAM prefix 0x${ramPrefix.toString(16)}  | key scan align=${align}`);
  console.log(`Output dir: ${outDir}`);

  const images = findImages(d);
  if (!images.length) { console.error('No images found (no header magic at any 64K slot).'); process.exit(2); }
  console.log(`Images: ${images.length} slot(s) -> ${images.map(i => hx(FLASH_BASE + i.base)).join(', ')}\n`);
  if (write) fs.mkdirSync(outDir, { recursive: true });

  const knownKeys = [];            // {off, bytes, profile}
  const keyByCipher = new Map();   // cipherHash -> {key, profile}
  const plainHashFirst = new Map();
  const written = []; let scans = 0;

  for (const im of images) {
    const src = d.subarray(im.base, im.base + im.len);
    const cHash = sha(src);
    const flash = FLASH_BASE + im.base;
    const tag = `slot @${hx(flash)} (file ${hx(im.base)}, len ${hx(im.len)})`;

    let found = keyByCipher.get(cHash) || null;
    if (!found) {
      // try already-known (key,profile) pairs first
      for (const k of knownKeys) if (validKey(k.bytes, k.profile, src, im.len)) { found = { key: k, profile: k.profile }; break; }
      // otherwise scan the dump under each cipher profile
      if (!found) {
        for (const p of PROFILES) {
          scans++;
          const hits = findKeyOffsets(d, src, im.len, ramPrefix, align, p);
          if (hits.length) {
            const k = { off: hits[0].off, bytes: Buffer.from(d.subarray(hits[0].off, hits[0].off + 16)), profile: p };
            knownKeys.push(k); found = { key: k, profile: p }; break;
          }
        }
      }
      if (found) keyByCipher.set(cHash, found);
    }

    if (!found) { console.log(`  ${tag}: NO key found -> skipped`); continue; }

    const { key, profile } = found;
    const plain = decryptImage(src, im.len, seedFromKeyBytes(key.bytes, profile.K));
    const pHash = sha(plain);
    const sp = plain.readUInt32LE(0), entry = plain.readUInt32LE(4);
    const dupOf = plainHashFirst.has(pHash) ? plainHashFirst.get(pHash) : null;
    if (dupOf === null) plainHashFirst.set(pHash, flash);

    const binName = `${stem}_decrypted_${addrTag(flash)}.bin`;
    const txtName = `${stem}_decrypted_${addrTag(flash)}.txt`;
    const info =
`Firmware decryption report
==========================
Generated      : ${new Date().toISOString()}  by decrypt_firmware.js
Input dump     : ${absDump}
Dump size      : ${d.length} bytes (${hx(d.length)})

Image slot     : flash ${hx(flash)}   (file offset ${hx(im.base)})
Image length   : ${im.len} bytes (${hx(im.len)})

Key (auto-found): ${key.bytes.toString('hex')}
Key location    : file ${hx(key.off)}  (flash ${hx(FLASH_BASE + key.off)})
Cipher profile  : ${profile.name}
  xorshift128 shifts 11/8/19; state = key^0x${profile.K.toString(16).toUpperCase()} (word-rotated)
  header words 128..143 (0x200..0x23F) left cleartext
  acceptance: 32-bit sum of decrypted words == ${hx(profile.TARGET)}  (passed)

Decrypted SP    : ${hx(sp)}
Decrypted entry : ${hx(entry)}  (Thumb=${(entry & 1) ? 'yes' : 'no'})

Ciphertext SHA-256 : ${cHash}
Plaintext  SHA-256 : ${pHash}
${dupOf !== null ? `Note            : identical decrypted image to slot ${hx(dupOf)} (redundant copy)\n` : ''}Output image    : ${binName}
`;
    if (write) {
      fs.writeFileSync(path.join(outDir, binName), plain);
      fs.writeFileSync(path.join(outDir, txtName), info);
      written.push(binName);
    }
    const dupNote = dupOf !== null ? `  (same plaintext as ${hx(dupOf)})` : '';
    console.log(`  ${tag}: key@${hx(key.off)}=${key.bytes.toString('hex')}  [${profile.name}]`);
    console.log(`        SP=${hx(sp)} entry=${hx(entry)}  plain-sha256=${pHash.slice(0, 24)}${dupNote}`);
    if (write) console.log(`        -> ${binName}  (+ .txt)`);
  }

  console.log(`\nKeys discovered: ${knownKeys.length} (${scans} dump scan(s))`);
  for (const k of knownKeys)
    console.log(`  ${k.bytes.toString('hex')}  @file ${hx(k.off)} (flash ${hx(FLASH_BASE + k.off)})  [${k.profile.name}]`);
  if (write) console.log(`\nWrote ${written.length} image(s) (+${written.length} .txt) to ${outDir}`);
}

main();

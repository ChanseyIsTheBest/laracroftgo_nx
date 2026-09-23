/* ---------------------------------------------------------------------------
 * lcgo_saveedit.c -- edit Lara Croft GO's save at boot from save.txt
 * (the battd_nx / sonicdash_nx design).
 *
 * THE SAVE -- all of it derived from this build's libil2cpp.so and
 * global-metadata.dat, and verified on a real save (five files, 455 values)
 *
 *   persistentDataPath + "/" + "SaveData" + <Type> + ".runtime", Type one of
 *   Int32, Boolean, Int64, Single, String (AbstractFilePersister, format
 *   "{0}{1}.{2}"). On this port persistentDataPath is the game folder.
 *
 *   Each file is one Dictionary<string, T>, written by
 *   PersisterLocalFile.WriteAllText through SimpleJSON (JSONClass.FromDictionary)
 *   and then ArmorySecurity's four Persistence filters:
 *
 *     ByteParser         string -> UTF-8 bytes
 *     LZMACompressor     7-Zip LZMA SDK (C#): 5 property bytes, 8-byte
 *                        little-endian size, then the range-coded stream
 *     RijndaelEncryptor  RijndaelManaged, CBC. Key =
 *                        Rfc2898DeriveBytes(PasswordHash,
 *                        ASCII(SaltKey)).GetBytes(32) -- PBKDF2-HMAC-SHA1, 1000
 *                        iterations (the (string, byte[]) constructor), so
 *                        AES-256. IV = ASCII(VIKey). Encrypt pads with Zeros;
 *                        decrypt uses None and keeps the zeros, which the LZMA
 *                        decoder never reaches because the header gives the size.
 *     StringEncoder      base64
 *
 *   The three KeySet strings are not literals: RijndaelEncryptor's constructor
 *   takes them from <PrivateImplementationDetails>{8ADB310A-...}, which keeps
 *   123 bytes at metadata offset 0x408038, decodes byte i as b ^ i ^ 0xAA, and
 *   slices them (index, offset, length): C() (4, 0x36, 0x11) PasswordHash,
 *   c() (5, 0x47, 0x10) VIKey, D() (6, 0x57, 0x24) SaltKey. The same table
 *   holds the four filter names, which is how the slicing was checked.
 *
 *   Reading goes back through the filters, then SimpleJSON's JSONNode.Parse.
 *   A file that fails is flagged corrupt by LocalFileRuntimePersistence -- which
 *   is why nothing here is written until it has been decoded again and matched.
 *
 * WHAT IS IN IT (SaveDataInt32 is the one that matters)
 *   Chapter_<c>_Locked / _Completed, Chapter_<c>_Level_<l>_Locked / _Completed
 *   / _HighestCheckpoint / _IsVaseBroken_<v>, IsGameFinished, Last_Played_*,
 *   <OutfitDesc name>_IsOutfitLocked, CurrentOutfitIndex (GameStructure). The
 *   other four hold shop and system bookkeeping.
 *
 *   GameStructure.ReadFromPersistence reads these straight into its chapter,
 *   level and outfit descriptors. After that, EnsureCompletedChaptersIntegrity
 *   only ever SETS completed flags, and RefreshPurchasedSkinPacks /
 *   RefreshPurchasedExpansions only ever CLEAR locks, so an edit is not undone
 *   at load. Relic outfits are unlocked by UnlockFragmentInLevel at the moment a
 *   relic is completed in play, not recomputed from the vases at load -- so
 *   breaking vases here does not unlock outfits; the outfit settings do that.
 *
 *   The hints (Complete Walkthrough) are a shop-inventory entry, not a flag:
 *   see apply_hints() for the derivation.
 *
 *   CurrentOutfitIndex indexes GameStructure.m_Outfits. The outfit keys sit in
 *   the save in that same order: WriteToPersistence and ReadFromPersistence
 *   both walk m_Outfits, and a Dictionary keeps insertion order. (The real save
 *   agrees: index 3, the default, is Classic, which is unlocked.)
 *
 * HOW IT EDITS
 *   save.txt is written once, every setting commented out. An uncommented
 *   setting is applied at EVERY launch while it stays uncommented. A value is
 *   replaced inside the JSON text as a token of the same kind the game wrote
 *   (a bare number, true/false, or a quoted string); every other byte of the
 *   text stays as it was. Only an add.<type>.<key> line adds a key: it goes at
 *   the end of the object, separated as SimpleJSON separates pairs (", "), into
 *   the file its type names, and never into a second file that already holds
 *   the key under another type. Every other setting only changes keys the save
 *   already has, and reports and skips the rest. Keys are never removed.
 *
 *   Re-encoding uses this file's own LZMA encoder, which writes literals only.
 *   That is a complete, valid LZMA stream -- any LZMA decoder, the game's
 *   included, reads it -- it just compresses less than the game's encoder
 *   (on the real save the files come out within a few percent of the game's
 *   own size -- its 16-byte dictionary finds few matches anyway). It
 *   uses no match distances at all, so the game's dictionary size (16 bytes in
 *   the header it writes) can never be exceeded. The property bytes are copied
 *   from the file being replaced.
 *
 * THE LIST
 *   After the edits, the end of save.txt is rewritten with every value in the
 *   save as a commented prop. line (list_refresh), so anything in the save can
 *   be edited by removing a '#'. Everything above the list is kept byte for
 *   byte, lines uncommented inside it are kept as written, and save.txt is only
 *   rewritten when the list's text changes. The list is left alone when a save
 *   file could not be read or written, so it never shows values that are not
 *   on the card.
 *
 * SAFETY, in the order it happens
 *   1. decode every save file; one that does not decode is reported and never
 *      written
 *   2. apply only the uncommented settings (add lines first, so the others can
 *      act on what they added); a file whose final text is unchanged is not
 *      written, so lines that cancel out cost nothing
 *   3. re-encode each changed file, then DECODE THE RESULT and compare it byte
 *      for byte with the text that was meant to be written, and check it still
 *      parses into the same keys in the same order. If any file fails, NO file
 *      is written
 *   4. keep the untouched original of each file once, as <name>.runtime.orig
 *   5. write <name>.runtime.tmp and rename it over, then commit the SD card
 *
 * Everything but the two thin Switch functions at the end is portable C: it
 * is built and tested on a PC against a real save (tools/test_saveedit.c), and
 * the re-encoded files are checked independently by tools/lcgo_save.py.
 * MIT.
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <strings.h>   /* strcasecmp, strncasecmp */
#include <sys/stat.h>
#include "lcgo_saveedit.h"

#ifdef __SWITCH__
#include <switch.h>
#include "config.h"
#include "util.h"
#endif

int lcgo_saveedit_verbose;

#define SE_MAX_FILE   (8u << 20)      /* a .runtime or save.txt larger than this is not ours */
#define SE_MAX_JSON   (16u << 20)     /* decoded size ceiling (the real save: 15 KB)        */
#define SE_MAX_SET    4096   /* the list alone has ~460 lines to uncomment */

/* ======================================================================= */
/* report: save.log (always) and debug.log (when the port's log is on)     */
/* ======================================================================= */
static char   g_rep[32768];
static size_t g_rep_n;
static int    g_rep_full;

static void se_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void se_log(const char *fmt, ...) {
  char line[768];
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, va);
  va_end(va);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
  line[n] = '\n'; line[n + 1] = 0;
  if (g_rep_n + (size_t)n + 1 < sizeof g_rep - 64) {
    memcpy(g_rep + g_rep_n, line, (size_t)n + 1);
    g_rep_n += (size_t)n + 1;
  } else if (!g_rep_full) {
    g_rep_full = 1;
    const char *t = "... (more lines not shown)\n";
    memcpy(g_rep + g_rep_n, t, strlen(t)); g_rep_n += strlen(t);
  }
  g_rep[g_rep_n] = 0;
#ifdef __SWITCH__
  debugPrintf("[saveedit] %s", line);
#else
  if (lcgo_saveedit_verbose) fputs(line, stdout);
#endif
}

/* ======================================================================= */
/* files                                                                   */
/* ======================================================================= */
/* Sized to the file: the six files are held at once, before the engine loads. */
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  long sz = -1;
  if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
  if (sz < 0 || (unsigned long)sz > SE_MAX_FILE || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char *b = malloc((size_t)sz + 1);
  const size_t n = b ? fread(b, 1, (size_t)sz, f) : 0;
  fclose(f);
  if (!b || n != (size_t)sz) { free(b); return NULL; }
  b[n] = 0;
  *len = n;
  return b;
}
static int write_file(const char *path, const void *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  const int ok = fwrite(b, 1, n, f) == n;
  return (fclose(f) == 0) && ok;
}
static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* ======================================================================= */
/* base64                                                                  */
/* ======================================================================= */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *in, size_t n, size_t *out_len) {
  const size_t on = (n + 2) / 3 * 4;
  char *o = malloc(on + 1);
  if (!o) return NULL;
  size_t k = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0)
                     | (i + 2 < n ? in[i + 2] : 0);
    o[k++] = B64[v >> 18 & 63];
    o[k++] = B64[v >> 12 & 63];
    o[k++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
    o[k++] = i + 2 < n ? B64[v & 63] : '=';
  }
  o[k] = 0;
  *out_len = k;
  return o;
}
static int b64_val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
/* Strict: whole groups of four, '=' only at the very end. */
static uint8_t *b64_decode(const char *s, size_t n, size_t *out_len) {
  if (!n || n % 4) return NULL;
  size_t pad = 0;
  if (s[n - 1] == '=') pad++;
  if (s[n - 2] == '=') pad++;
  uint8_t *o = malloc(n / 4 * 3);
  if (!o) return NULL;
  size_t k = 0;
  for (size_t i = 0; i < n; i += 4) {
    int v[4];
    for (int j = 0; j < 4; j++) {
      const int last = i + 4 == n;
      if (last && s[i + j] == '=' && j >= 4 - (int)pad) { v[j] = 0; continue; }
      if ((v[j] = b64_val((unsigned char)s[i + j])) < 0) { free(o); return NULL; }
    }
    const uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
    o[k++] = (uint8_t)(w >> 16);
    o[k++] = (uint8_t)(w >> 8);
    o[k++] = (uint8_t)w;
  }
  *out_len = k - pad;
  return o;
}

/* ======================================================================= */
/* SHA-1, HMAC-SHA1, PBKDF2 -- for Rfc2898DeriveBytes                      */
/* ======================================================================= */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } Sha1;

static uint32_t rol32(uint32_t x, int r) { return x << r | x >> (32 - r); }
static void sha1_block(Sha1 *s, const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
    else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
    else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
    const uint32_t t = rol32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol32(b, 30); b = a; a = t;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
  static const uint32_t iv[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
  memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}
static void sha1_update(Sha1 *s, const uint8_t *p, size_t n) {
  s->len += n;
  while (n) {
    const size_t t = 64 - s->n < n ? 64 - s->n : n;
    memcpy(s->buf + s->n, p, t); s->n += t; p += t; n -= t;
    if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
  }
}
static void sha1_final(Sha1 *s, uint8_t out[20]) {
  const uint64_t bits = s->len * 8;
  const uint8_t one = 0x80, zero = 0;
  sha1_update(s, &one, 1);
  while (s->n != 56) sha1_update(s, &zero, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha1_update(s, l, 8);
  for (int i = 0; i < 5; i++) {
    out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
  }
}
static void hmac_sha1(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t out[20]) {
  uint8_t k[64] = { 0 }, ipad[64], opad[64], inner[20];
  Sha1 s;
  if (kl > 64) { sha1_init(&s); sha1_update(&s, key, kl); sha1_final(&s, k); }
  else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
  sha1_init(&s); sha1_update(&s, ipad, 64); sha1_update(&s, msg, ml); sha1_final(&s, inner);
  sha1_init(&s); sha1_update(&s, opad, 64); sha1_update(&s, inner, 20); sha1_final(&s, out);
}
static void pbkdf2_sha1(const uint8_t *pw, size_t pl, const uint8_t *salt, size_t sl,
                        unsigned iters, uint8_t *out, size_t outlen) {
  uint8_t msg[128], u[20], t[20];
  if (sl + 4 > sizeof msg) return;
  memcpy(msg, salt, sl);
  for (uint32_t block = 1; outlen; block++) {
    msg[sl] = (uint8_t)(block >> 24); msg[sl + 1] = (uint8_t)(block >> 16);
    msg[sl + 2] = (uint8_t)(block >> 8); msg[sl + 3] = (uint8_t)block;
    hmac_sha1(pw, pl, msg, sl + 4, u);
    memcpy(t, u, 20);
    for (unsigned i = 1; i < iters; i++) {
      hmac_sha1(pw, pl, u, 20, u);
      for (int j = 0; j < 20; j++) t[j] ^= u[j];
    }
    const size_t n = outlen < 20 ? outlen : 20;
    memcpy(out, t, n); out += n; outlen -= n;
  }
}

/* ======================================================================= */
/* AES-256 (FIPS-197), CBC                                                 */
/* ======================================================================= */
static const uint8_t SBOX[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static uint8_t INV_SBOX[256];

typedef struct { uint8_t rk[240]; } Aes256;   /* 15 round keys */

static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1 ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
  return r;
}
static void aes256_init(Aes256 *a, const uint8_t key[32]) {
  if (!INV_SBOX[0x63] && !INV_SBOX[0x7c]) for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
  memcpy(a->rk, key, 32);
  uint8_t rcon = 1;
  for (int i = 8; i < 60; i++) {
    uint8_t t[4];
    memcpy(t, a->rk + 4 * (i - 1), 4);
    if (i % 8 == 0) {
      const uint8_t u = t[0];
      t[0] = SBOX[t[1]] ^ rcon; t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[u];
      rcon = xt(rcon);
    } else if (i % 8 == 4) {
      for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
    }
    for (int j = 0; j < 4; j++) a->rk[4 * i + j] = a->rk[4 * (i - 8) + j] ^ t[j];
  }
}
static void add_rk(uint8_t s[16], const uint8_t *rk) { for (int i = 0; i < 16; i++) s[i] ^= rk[i]; }
static void aes256_encrypt_block(const Aes256 *a, uint8_t s[16]) {
  add_rk(s, a->rk);
  for (int r = 1; r <= 14; r++) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];   /* SubBytes + ShiftRows */
    if (r < 14)
      for (int c = 0; c < 4; c++) {                                        /* MixColumns */
        uint8_t *m = t + 4 * c;
        const uint8_t a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3], all = a0 ^ a1 ^ a2 ^ a3;
        m[0] ^= all ^ xt(a0 ^ a1); m[1] ^= all ^ xt(a1 ^ a2);
        m[2] ^= all ^ xt(a2 ^ a3); m[3] ^= all ^ xt(a3 ^ a0);
      }
    memcpy(s, t, 16);
    add_rk(s, a->rk + 16 * r);
  }
}
static void aes256_decrypt_block(const Aes256 *a, uint8_t s[16]) {
  add_rk(s, a->rk + 16 * 14);
  for (int r = 13; r >= 0; r--) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = INV_SBOX[s[i]];   /* InvShiftRows + InvSubBytes */
    add_rk(t, a->rk + 16 * r);
    if (r > 0)
      for (int c = 0; c < 4; c++) {                                            /* InvMixColumns */
        uint8_t *m = t + 4 * c;
        const uint8_t a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
        m[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        m[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        m[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        m[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
      }
    memcpy(s, t, 16);
  }
}

/* The game's KeySet, recovered from the string table described at the top. */
static const char SE_PASSWORD[] = "bW89ej!4tSr&u?U&?";                       /* PasswordHash */
static const char SE_IV[]       = "@5027edS453Fu28J";                         /* VIKey        */
static const char SE_SALT[]     = "F896D1B0-474B-41F7-84E6-1FCD28A34358";     /* SaltKey      */

static const Aes256 *save_cipher(void) {
  static Aes256 aes;
  static int ready;
  if (!ready) {
    uint8_t key[32];
    pbkdf2_sha1((const uint8_t *)SE_PASSWORD, sizeof SE_PASSWORD - 1,
                (const uint8_t *)SE_SALT, sizeof SE_SALT - 1, 1000, key, sizeof key);
    aes256_init(&aes, key);
    ready = 1;
  }
  return &aes;
}
/* CBC in place; n is a multiple of 16. */
static void cbc_decrypt(uint8_t *p, size_t n) {
  const Aes256 *a = save_cipher();
  uint8_t prev[16], cur[16];
  memcpy(prev, SE_IV, 16);
  for (size_t i = 0; i < n; i += 16) {
    memcpy(cur, p + i, 16);
    aes256_decrypt_block(a, p + i);
    for (int j = 0; j < 16; j++) p[i + j] ^= prev[j];
    memcpy(prev, cur, 16);
  }
}
static void cbc_encrypt(uint8_t *p, size_t n) {
  const Aes256 *a = save_cipher();
  const uint8_t *prev = (const uint8_t *)SE_IV;
  for (size_t i = 0; i < n; i += 16) {
    for (int j = 0; j < 16; j++) p[i + j] ^= prev[j];
    aes256_encrypt_block(a, p + i);
    prev = p + i;
  }
}

/* ======================================================================= */
/* LZMA: full decoder, literal-only encoder                                */
/* ======================================================================= */
#define LZ_TOP        (1u << 24)
#define LZ_PROB_INIT  1024
#define LZ_STATES     12

typedef struct { const uint8_t *in; size_t n, pos; uint32_t range, code; int err; } RDec;

static uint8_t rd_in(RDec *r) {
  if (r->pos >= r->n) { r->err = 1; return 0; }
  return r->in[r->pos++];
}
static unsigned rd_bit(RDec *r, uint16_t *p) {
  const uint32_t bound = (r->range >> 11) * *p;
  unsigned b;
  if (r->code < bound) { *p = (uint16_t)(*p + ((2048 - *p) >> 5)); r->range = bound; b = 0; }
  else { *p = (uint16_t)(*p - (*p >> 5)); r->code -= bound; r->range -= bound; b = 1; }
  if (r->range < LZ_TOP) { r->range <<= 8; r->code = r->code << 8 | rd_in(r); }
  return b;
}
static uint32_t rd_direct(RDec *r, int nbits) {
  uint32_t res = 0;
  while (nbits-- > 0) {
    r->range >>= 1;
    r->code -= r->range;
    const uint32_t t = 0u - (r->code >> 31);
    r->code += r->range & t;
    if (r->code == r->range) r->err = 1;
    if (r->range < LZ_TOP) { r->range <<= 8; r->code = r->code << 8 | rd_in(r); }
    res = (res << 1) + (t + 1);
  }
  return res;
}
static unsigned bt_dec(RDec *r, uint16_t *p, int nb) {
  unsigned m = 1;
  for (int i = 0; i < nb; i++) m = (m << 1) + rd_bit(r, &p[m]);
  return m - (1u << nb);
}
static unsigned bt_rev(RDec *r, uint16_t *p, int nb) {
  unsigned m = 1, s = 0;
  for (int i = 0; i < nb; i++) { const unsigned b = rd_bit(r, &p[m]); m = (m << 1) + b; s |= b << i; }
  return s;
}

typedef struct { uint16_t choice, choice2, low[16][8], mid[16][8], high[256]; } LenDec;
static unsigned len_dec(RDec *r, LenDec *l, unsigned ps) {
  if (!rd_bit(r, &l->choice)) return bt_dec(r, l->low[ps], 3);
  if (!rd_bit(r, &l->choice2)) return 8 + bt_dec(r, l->mid[ps], 3);
  return 16 + bt_dec(r, l->high, 8);
}

typedef struct {
  uint16_t is_match[LZ_STATES][16], is_rep[LZ_STATES], is_rep_g0[LZ_STATES],
           is_rep_g1[LZ_STATES], is_rep_g2[LZ_STATES], is_rep0_long[LZ_STATES][16];
  uint16_t pos_slot[4][64], pos_dec[115], align[16];
  LenDec len, rep_len;
} LzModel;

static void probs_init(uint16_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = LZ_PROB_INIT; }

static int lz_props(uint8_t d, unsigned *lc, unsigned *lp, unsigned *pb) {
  if (d >= 9 * 5 * 5) return 0;
  *lc = d % 9; d /= 9; *lp = d % 5; *pb = d / 5;
  return *lc <= 8 && *lp <= 4 && *pb <= 4;
}

/* A .lzma ("LZMA alone") buffer as LZMAUtils.Decompress reads it: 5 property
 * bytes, 8-byte size, stream. Decodes exactly `size` bytes. Returns the malloc'd
 * output (NUL-terminated) or NULL with a reason. */
static uint8_t *lzma_decode(const uint8_t *in, size_t n, size_t *out_n, const char **why) {
  unsigned lc, lp, pb;
  if (n < 13 + 5) { *why = "LZMA data too short"; return NULL; }
  if (!lz_props(in[0], &lc, &lp, &pb)) { *why = "LZMA properties invalid"; return NULL; }
  uint64_t usize = 0;
  for (int i = 7; i >= 0; i--) usize = usize << 8 | in[5 + i];
  if (usize > SE_MAX_JSON) { *why = "LZMA size implausible"; return NULL; }

  uint8_t *out = malloc((size_t)usize + 1);
  LzModel *m = malloc(sizeof *m);
  uint16_t *lit = malloc((size_t)0x300 << (lc + lp) << 1);
  if (!out || !m || !lit) { free(out); free(m); free(lit); *why = "out of memory"; return NULL; }
  probs_init((uint16_t *)m, sizeof *m / 2);
  probs_init(lit, (size_t)0x300 << (lc + lp));

  RDec r = { in + 13, n - 13, 0, 0xFFFFFFFFu, 0, 0 };
  if (rd_in(&r) != 0) { *why = "LZMA stream does not start with 0"; goto fail; }
  for (int i = 0; i < 4; i++) r.code = r.code << 8 | rd_in(&r);
  if (r.code == r.range) { *why = "LZMA stream corrupt"; goto fail; }

  const unsigned pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
  unsigned state = 0;
  uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
  size_t pos = 0;
  while (pos < usize) {
    const unsigned ps = (unsigned)pos & pb_mask;
    if (!rd_bit(&r, &m->is_match[state][ps])) {
      const unsigned prev = pos ? out[pos - 1] : 0;
      uint16_t *p = lit + 0x300 * (((pos & lp_mask) << lc) + (prev >> (8 - lc)));
      unsigned sym = 1;
      if (state >= 7) {
        unsigned mb = out[pos - rep0 - 1];
        do {
          const unsigned mbit = (mb >> 7) & 1;
          mb <<= 1;
          const unsigned b = rd_bit(&r, &p[((1 + mbit) << 8) + sym]);
          sym = sym << 1 | b;
          if (mbit != b) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) sym = sym << 1 | rd_bit(&r, &p[sym]);
      out[pos++] = (uint8_t)sym;
      state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
    } else {
      unsigned len;
      if (rd_bit(&r, &m->is_rep[state])) {
        if (!pos) { *why = "LZMA repeat before any data"; goto fail; }
        if (!rd_bit(&r, &m->is_rep_g0[state])) {
          if (!rd_bit(&r, &m->is_rep0_long[state][ps])) {
            state = state < 7 ? 9 : 11;
            out[pos] = out[pos - rep0 - 1];
            pos++;
            if (r.err) { *why = "LZMA stream truncated"; goto fail; }
            continue;
          }
        } else {
          uint32_t dist;
          if (!rd_bit(&r, &m->is_rep_g1[state])) dist = rep1;
          else {
            if (!rd_bit(&r, &m->is_rep_g2[state])) dist = rep2;
            else { dist = rep3; rep3 = rep2; }
            rep2 = rep1;
          }
          rep1 = rep0; rep0 = dist;
        }
        len = len_dec(&r, &m->rep_len, ps);
        state = state < 7 ? 8 : 11;
      } else {
        rep3 = rep2; rep2 = rep1; rep1 = rep0;
        len = len_dec(&r, &m->len, ps);
        state = state < 7 ? 7 : 10;
        const unsigned ls = len < 3 ? len : 3;
        const unsigned slot = bt_dec(&r, m->pos_slot[ls], 6);
        if (slot < 4) rep0 = slot;
        else {
          const int nd = (int)(slot >> 1) - 1;
          rep0 = (2 | (slot & 1)) << nd;
          if (slot < 14) rep0 += bt_rev(&r, m->pos_dec + rep0 - slot, nd);
          else {
            rep0 += rd_direct(&r, nd - 4) << 4;
            rep0 += bt_rev(&r, m->align, 4);
          }
        }
        if (rep0 == 0xFFFFFFFFu) { *why = "LZMA end marker before the stated size"; goto fail; }
      }
      len += 2;
      if (rep0 >= pos) { *why = "LZMA distance beyond the data"; goto fail; }
      if (len > usize - pos) { *why = "LZMA data runs past the stated size"; goto fail; }
      for (unsigned i = 0; i < len; i++, pos++) out[pos] = out[pos - rep0 - 1];
    }
    if (r.err) { *why = "LZMA stream truncated or corrupt"; goto fail; }
  }
  free(m); free(lit);
  out[usize] = 0;
  *out_n = (size_t)usize;
  return out;
fail:
  free(out); free(m); free(lit);
  return NULL;
}

typedef struct { uint64_t low; uint32_t range; uint8_t cache; uint64_t cache_size; uint8_t *o; size_t n, cap; int oom; } REnc;

static void re_out(REnc *e, uint8_t b) {
  if (e->n == e->cap) {
    const size_t nc = e->cap ? e->cap * 2 : 4096;
    uint8_t *no = realloc(e->o, nc);
    if (!no) { e->oom = 1; return; }
    e->o = no; e->cap = nc;
  }
  e->o[e->n++] = b;
}
/* LzmaEnc's RangeEnc_ShiftLow, verbatim in effect. */
static void re_shift_low(REnc *e) {
  if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
    uint8_t temp = e->cache;
    do { re_out(e, (uint8_t)(temp + (uint8_t)(e->low >> 32))); temp = 0xFF; } while (--e->cache_size != 0);
    e->cache = (uint8_t)((uint32_t)e->low >> 24);
  }
  e->cache_size++;
  e->low = (uint32_t)((uint32_t)e->low << 8);
}
static void re_bit(REnc *e, uint16_t *p, unsigned bit) {
  const uint32_t bound = (e->range >> 11) * *p;
  if (!bit) { e->range = bound; *p = (uint16_t)(*p + ((2048 - *p) >> 5)); }
  else { e->low += bound; e->range -= bound; *p = (uint16_t)(*p - (*p >> 5)); }
  while (e->range < LZ_TOP) { e->range <<= 8; re_shift_low(e); }
}

/* Literal-only LZMA with the given property bytes (lc/lp/pb taken from the
 * first, the dictionary size copied as is), the size in the header, no end
 * marker -- the layout LZMAUtils.Compress writes and Decompress reads. With no
 * matches the coder never leaves state 0, so every byte is is_match[0][pos] = 0
 * followed by a plain literal. */
static uint8_t *lzma_encode_literals(const uint8_t *in, size_t n, const uint8_t props[5], size_t *out_n) {
  unsigned lc, lp, pb;
  if (!lz_props(props[0], &lc, &lp, &pb)) return NULL;
  uint16_t is_match[16], *lit = malloc((size_t)0x300 << (lc + lp) << 1);
  if (!lit) return NULL;
  probs_init(is_match, 16);
  probs_init(lit, (size_t)0x300 << (lc + lp));
  REnc e = { 0, 0xFFFFFFFFu, 0, 1, NULL, 0, 0, 0 };
  for (int i = 0; i < 5; i++) re_out(&e, props[i]);
  for (int i = 0; i < 8; i++) re_out(&e, (uint8_t)((uint64_t)n >> (8 * i)));
  const unsigned pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
  for (size_t pos = 0; pos < n; pos++) {
    re_bit(&e, &is_match[pos & pb_mask], 0);
    const unsigned prev = pos ? in[pos - 1] : 0;
    uint16_t *p = lit + 0x300 * (((pos & lp_mask) << lc) + (prev >> (8 - lc)));
    unsigned sym = in[pos] | 0x100u;
    do { re_bit(&e, &p[sym >> 8], (sym >> 7) & 1); sym <<= 1; } while (sym < 0x10000u);
  }
  for (int i = 0; i < 5; i++) re_shift_low(&e);
  free(lit);
  if (e.oom) { free(e.o); return NULL; }
  *out_n = e.n;
  return e.o;
}

/* ======================================================================= */
/* the container                                                           */
/* ======================================================================= */
int lcgo_save_decode(const char *text, size_t n, char **json, size_t *jlen,
                     unsigned char props[5], char *err, size_t errsz) {
  if (n >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) { text += 3; n -= 3; }
  while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) n--;
  size_t cn = 0;
  uint8_t *c = b64_decode(text, n, &cn);
  if (!c) { snprintf(err, errsz, "not base64 text"); return -1; }
  if (!cn || cn % 16) { free(c); snprintf(err, errsz, "not whole AES blocks (%zu bytes)", cn); return -1; }
  cbc_decrypt(c, cn);
  const char *why = "?";
  size_t on = 0;
  uint8_t *o = lzma_decode(c, cn, &on, &why);
  if (o) memcpy(props, c, 5);
  free(c);
  if (!o) { snprintf(err, errsz, "%s (wrong key, or not a save file)", why); return -1; }
  *json = (char *)o;
  *jlen = on;
  return 0;
}

char *lcgo_save_encode(const char *json, size_t jlen, const unsigned char props[5], size_t *out_len) {
  size_t zn = 0;
  uint8_t *z = lzma_encode_literals((const uint8_t *)json, jlen, props, &zn);
  if (!z) return NULL;
  const size_t padded = (zn + 15) / 16 * 16;                 /* PaddingMode.Zeros */
  uint8_t *c = realloc(z, padded ? padded : 16);
  if (!c) { free(z); return NULL; }
  memset(c + zn, 0, padded - zn);
  cbc_encrypt(c, padded);
  char *t = b64_encode(c, padded, out_len);
  free(c);
  return t;
}

/* ======================================================================= */
/* the JSON: one flat object, exactly as SimpleJSON writes it              */
/* ======================================================================= */
typedef struct { size_t ks, ke, vs, ve; } Pair;   /* key: between the quotes; value: the whole token */

static int json_skip_string(const char *j, size_t n, size_t *i) {   /* at the opening quote */
  for (size_t k = *i + 1; k < n; k++) {
    if (j[k] == '\\') { k++; continue; }
    if (j[k] == '"') { *i = k + 1; return 1; }
  }
  return 0;
}
/* Returns the pair count, or -1 with a reason. *out is malloc'd (may be NULL for 0). */
static int json_scan(const char *j, size_t n, Pair **out, const char **why) {
  size_t i = 0, cap = 0;
  int cnt = 0;
  Pair *p = NULL;
#define WS() while (i < n && (j[i] == ' ' || j[i] == '\t' || j[i] == '\r' || j[i] == '\n')) i++
  *out = NULL;
  WS();
  if (i >= n || j[i] != '{') { *why = "does not start with '{'"; return -1; }
  i++; WS();
  if (i < n && j[i] == '}') { i++; goto end; }
  for (;;) {
    if (i >= n || j[i] != '"') { *why = "expected a quoted key"; goto bad; }
    const size_t ks = i + 1;
    if (!json_skip_string(j, n, &i)) { *why = "unterminated key"; goto bad; }
    const size_t ke = i - 1;
    WS();
    if (i >= n || j[i] != ':') { *why = "expected ':'"; goto bad; }
    i++; WS();
    const size_t vs = i;
    if (i < n && j[i] == '"') {
      if (!json_skip_string(j, n, &i)) { *why = "unterminated string value"; goto bad; }
    } else if (i < n && (j[i] == '{' || j[i] == '[')) {
      *why = "nested value (this save is flat)"; goto bad;
    } else {
      while (i < n && j[i] != ',' && j[i] != '}' && j[i] != ' ' && j[i] != '\t' && j[i] != '\r' && j[i] != '\n') i++;
      if (i == vs) { *why = "empty value"; goto bad; }
    }
    const size_t ve = i;
    if ((size_t)cnt == cap) {
      cap = cap ? cap * 2 : 256;
      Pair *np = realloc(p, cap * sizeof *p);
      if (!np) { *why = "out of memory"; goto bad; }
      p = np;
    }
    p[cnt++] = (Pair){ ks, ke, vs, ve };
    WS();
    if (i < n && j[i] == ',') { i++; WS(); continue; }
    if (i < n && j[i] == '}') { i++; break; }
    *why = "expected ',' or '}'"; goto bad;
  }
end:
  WS();
  if (i != n) { *why = "text after the closing '}'"; goto bad; }
  *out = p;
  return cnt;
bad:
  free(p);
  return -1;
#undef WS
}

/* ======================================================================= */
/* the five files                                                          */
/* ======================================================================= */
typedef enum { T_I32, T_BOOL, T_I64, T_F32, T_STR, T_N } SType;
static const char *const k_name[T_N] = { "SaveDataInt32", "SaveDataBoolean", "SaveDataInt64",
                                          "SaveDataSingle", "SaveDataString" };

typedef struct {
  int present, ok, changed;
  char path[512];
  char *raw; size_t raw_n;          /* the file as read, for .orig */
  unsigned char props[5];
  char *j; size_t jn;               /* decoded JSON, edited in place */
  char *j0; size_t j0n;             /* decoded JSON as loaded: "changed" = differs from this */
  Pair *p; int n;
  char *out; size_t out_n;          /* re-encoded file text */
} SFile;

static int key_is(const SFile *f, int i, const char *k) {
  const size_t l = strlen(k);
  return f->p[i].ke - f->p[i].ks == l && !memcmp(f->j + f->p[i].ks, k, l);
}
static void token_of(const SFile *f, int i, char *out, size_t n) {
  const size_t l = f->p[i].ve - f->p[i].vs;
  snprintf(out, n, "%.*s", (int)(l < n - 1 ? l : n - 1), f->j + f->p[i].vs);
}
static void key_of(const SFile *f, int i, char *out, size_t n) {
  const size_t l = f->p[i].ke - f->p[i].ks;
  snprintf(out, n, "%.*s", (int)(l < n - 1 ? l : n - 1), f->j + f->p[i].ks);
}
/* Replace pair i's value token. 1 changed, 0 already equal, -1 out of memory. */
static int set_token(SFile *f, int i, const char *tok) {
  const size_t tl = strlen(tok), vs = f->p[i].vs, ve = f->p[i].ve, ol = ve - vs;
  if (ol == tl && !memcmp(f->j + vs, tok, tl)) return 0;
  const size_t nn = f->jn - ol + tl;
  char *nj = malloc(nn + 1);
  if (!nj) return -1;
  memcpy(nj, f->j, vs);
  memcpy(nj + vs, tok, tl);
  memcpy(nj + vs + tl, f->j + ve, f->jn - ve + 1);            /* with the NUL */
  free(f->j); f->j = nj; f->jn = nn;
  const long d = (long)tl - (long)ol;
  f->p[i].ve = vs + tl;
  for (int k = i + 1; k < f->n; k++) {
    f->p[k].ks += d; f->p[k].ke += d; f->p[k].vs += d; f->p[k].ve += d;
  }
  f->changed = 1;
  return 1;
}

/* Add "key":tok at the end of the object, the way SimpleJSON separates pairs
 * (", "). The key is plain printable ASCII without quotes or backslashes, so it
 * needs no escaping. 1 added, -1 out of memory. */
static int append_pair(SFile *f, const char *key, const char *tok) {
  size_t close = f->jn;
  while (close > 0 && f->j[close - 1] != '}') close--;
  if (!close) return -1;                                   /* json_scan saw a '}' */
  close--;                                                 /* index of the '}' */
  const char *sep = f->n ? ", " : "";
  const size_t kl = strlen(key), tl = strlen(tok), il = strlen(sep) + kl + 3 + tl;
  Pair *np = realloc(f->p, (size_t)(f->n + 1) * sizeof *np);
  if (!np) return -1;
  f->p = np;
  char *nj = malloc(f->jn + il + 1);
  if (!nj) return -1;
  memcpy(nj, f->j, close);
  size_t o = close;
  o += (size_t)sprintf(nj + o, "%s\"", sep);
  const size_t ks = o;
  memcpy(nj + o, key, kl); o += kl;
  const size_t ke = o;
  nj[o++] = '"'; nj[o++] = ':';
  const size_t vs = o;
  memcpy(nj + o, tok, tl); o += tl;
  memcpy(nj + o, f->j + close, f->jn - close + 1);        /* '}', any trailing space, NUL */
  free(f->j); f->j = nj; f->jn += il;
  f->p[f->n++] = (Pair){ ks, ke, vs, vs + tl };
  f->changed = 1;
  return 1;
}

static void sfile_free(SFile *f) {
  free(f->raw); free(f->j); free(f->j0); free(f->p); free(f->out);
  memset(f, 0, sizeof *f);
}

static void sfile_load(SFile *f, const char *dir, SType t) {
  snprintf(f->path, sizeof f->path, "%s/%s.runtime", dir, k_name[t]);
  f->raw = read_file(f->path, &f->raw_n);
  if (!f->raw) return;
  f->present = 1;
  char err[160];
  if (lcgo_save_decode(f->raw, f->raw_n, &f->j, &f->jn, f->props, err, sizeof err)) {
    se_log("%s.runtime: not decodable (%s) -- left alone", k_name[t], err);
    return;
  }
  const char *why = "?";
  if ((f->n = json_scan(f->j, f->jn, &f->p, &why)) < 0) {
    se_log("%s.runtime: decoded, but not in the form the game writes (%s) -- left alone", k_name[t], why);
    f->n = 0;
    return;
  }
  if (!(f->j0 = malloc(f->jn + 1))) { se_log("%s.runtime: out of memory -- left alone", k_name[t]); return; }
  memcpy(f->j0, f->j, f->jn + 1);
  f->j0n = f->jn;
  f->ok = 1;
}
static int sfile_differs(const SFile *f) {
  return f->ok && (f->jn != f->j0n || memcmp(f->j, f->j0, f->jn));
}

/* ======================================================================= */
/* values                                                                  */
/* ======================================================================= */
static int parse_bool(const char *v, int *out) {
  if (!strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"))  { *out = 1; return 1; }
  if (!strcasecmp(v, "false") || !strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) { *out = 0; return 1; }
  return 0;
}
static int is_integer(const char *v, int bits) {
  const char *p = v;
  if (*p == '-') p++;
  if (!*p || strlen(p) > 19) return 0;
  for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) return 0;
  if (p != v && !strcmp(p, "0")) return 0;                         /* "-0" */
  if (strlen(p) > 1 && *p == '0') return 0;                        /* leading zeros */
  char *e;
  const long long x = strtoll(v, &e, 10);
  if (*e) return 0;
  if (bits == 32) return x >= INT32_MIN && x <= INT32_MAX;
  /* 64: strtoll saturates; reject the saturated ends unless typed exactly */
  if ((x == INT64_MAX && strcmp(v, "9223372036854775807")) ||
      (x == INT64_MIN && strcmp(v, "-9223372036854775808"))) return 0;
  return 1;
}
static int is_decimal(const char *v) {
  const char *p = v;
  if (*p == '-') p++;
  if (!isdigit((unsigned char)*p) || strlen(v) > 24) return 0;
  while (isdigit((unsigned char)*p)) p++;
  if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return 0; while (isdigit((unsigned char)*p)) p++; }
  return *p == 0;
}
static int is_plain_text(const char *v) {
  for (; *v; v++) if ((unsigned char)*v < 0x20 || (unsigned char)*v > 0x7E || *v == '"' || *v == '\\') return 0;
  return 1;
}
/* The token to write for value v in a file of type t, or 0 with a reason. */
static int make_token(SType t, const char *v, char *tok, size_t n, const char **why) {
  int b;
  switch (t) {
    case T_I32:  if (!is_integer(v, 32)) { *why = "a whole number (32-bit)"; return 0; } snprintf(tok, n, "%s", v); return 1;
    case T_I64:  if (!is_integer(v, 64)) { *why = "a whole number (64-bit)"; return 0; } snprintf(tok, n, "%s", v); return 1;
    case T_F32:  if (!is_decimal(v))     { *why = "a number like 0 or 1.5"; return 0; }   snprintf(tok, n, "%s", v); return 1;
    case T_BOOL: if (!parse_bool(v, &b)) { *why = "true or false"; return 0; }            snprintf(tok, n, "%s", b ? "true" : "false"); return 1;
    case T_STR:
      if (!strcmp(v, "\"\"")) { snprintf(tok, n, "\"\""); return 1; }     /* "" = empty text */
      if (!is_plain_text(v) || strlen(v) + 3 > n) { *why = "one line of plain text without quotes or backslashes (\"\" for empty)"; return 0; }
      snprintf(tok, n, "\"%s\"", v); return 1;
    default: *why = "?"; return 0;
  }
}
/* add.<type>: the file a new value goes into. */
static int type_by_name(const char *s, size_t n) {
  static const struct { const char *name; SType t; } k[] = {
    { "int", T_I32 }, { "int32", T_I32 }, { "long", T_I64 }, { "int64", T_I64 },
    { "bool", T_BOOL }, { "boolean", T_BOOL }, { "float", T_F32 }, { "single", T_F32 },
    { "string", T_STR },
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (strlen(k[i].name) == n && !strncasecmp(s, k[i].name, n)) return (int)k[i].t;
  return -1;
}
static int is_plain_key(const char *k) {
  if (!*k || strlen(k) > 200) return 0;
  return is_plain_text(k);
}

/* The existing token must be of the kind the file holds, or we do not touch it. */
static int token_kind_ok(const SFile *f, int i, SType t) {
  const char c = f->j[f->p[i].vs];
  return t == T_STR ? c == '"' : c != '"';
}

/* ======================================================================= */
/* the Int32 keys GameStructure writes                                     */
/* ======================================================================= */
enum { K_OTHER, K_CH_LOCKED, K_CH_DONE, K_LV_LOCKED, K_LV_DONE, K_LV_VASE };
typedef struct { int kind, ch, lv; } KInfo;

static const char *eat_num(const char *p, const char *end, int *out) {
  int v = 0, d = 0;
  while (p < end && isdigit((unsigned char)*p) && d < 4) { v = v * 10 + (*p - '0'); p++; d++; }
  if (!d || (p < end && isdigit((unsigned char)*p))) return NULL;
  *out = v;
  return p;
}
static int eat_lit(const char **p, const char *end, const char *lit) {
  const size_t l = strlen(lit);
  if ((size_t)(end - *p) < l || memcmp(*p, lit, l)) return 0;
  *p += l;
  return 1;
}
/* "Chapter_{0}_Locked", "Chapter_{0}_Completed", "Chapter_{0}_Level_{1}_Locked",
 * "Chapter_{0}_Level_{1}_Completed", "Chapter_{0}_Level_{1}_IsVaseBroken_{2}" --
 * GameStructure's own format strings, matched exactly. */
static KInfo classify(const SFile *f, int i) {
  KInfo k = { K_OTHER, -1, -1 };
  const char *p = f->j + f->p[i].ks, *end = f->j + f->p[i].ke;
  int ch, lv, v;
  if (!eat_lit(&p, end, "Chapter_") || !(p = eat_num(p, end, &ch))) return k;
  const char *q = p;
  if (eat_lit(&q, end, "_Locked") && q == end)    { k.kind = K_CH_LOCKED; k.ch = ch; return k; }
  q = p;
  if (eat_lit(&q, end, "_Completed") && q == end) { k.kind = K_CH_DONE;   k.ch = ch; return k; }
  if (!eat_lit(&p, end, "_Level_") || !(p = eat_num(p, end, &lv))) return k;
  q = p;
  if (eat_lit(&q, end, "_Locked") && q == end)    { k.kind = K_LV_LOCKED; k.ch = ch; k.lv = lv; return k; }
  q = p;
  if (eat_lit(&q, end, "_Completed") && q == end) { k.kind = K_LV_DONE;   k.ch = ch; k.lv = lv; return k; }
  q = p;
  if (eat_lit(&q, end, "_IsVaseBroken_") && (q = eat_num(q, end, &v)) && q == end) {
    k.kind = K_LV_VASE; k.ch = ch; k.lv = lv; return k;
  }
  return k;
}

/* ======================================================================= */
/* settings                                                                */
/* ======================================================================= */
typedef struct { char *k, *v; } Setting;

enum { F_UNLOCKED, F_COMPLETED, F_VASES };
static int progress_field(const char *s) {
  if (!strcasecmp(s, "unlocked"))  return F_UNLOCKED;
  if (!strcasecmp(s, "completed")) return F_COMPLETED;
  if (!strcasecmp(s, "vases"))     return F_VASES;
  return -1;
}

/* Set every progress value in scope. ch -1 = every chapter; lv -1 = the whole
 * chapter (its own flags and every level in it).
 *   unlocked  true : Locked = 0 for the levels, and for their chapter(s)
 *             false: Locked = 1 for the levels; the chapter too if whole
 *   completed true : Completed = 1 for the levels (and chapter if whole), and
 *                    Locked = 0 for the same levels and their chapter(s)
 *             false: Completed = 0 for the levels (and chapter if whole)
 *   vases     on   : IsVaseBroken = on for every vase of the levels
 * Returns changed count, or -1 if nothing in the save is in scope. */
static int apply_progress(SFile *f, int ch, int lv, int field, int on, int *touched) {
  int changed = 0, seen = 0;
  *touched = 0;
  for (int i = 0; i < f->n; i++) {
    const KInfo k = classify(f, i);
    if (k.kind == K_OTHER || (ch >= 0 && k.ch != ch)) continue;
    const int is_chapter = k.kind == K_CH_LOCKED || k.kind == K_CH_DONE;
    if (lv >= 0 && !is_chapter && k.lv != lv) continue;
    if (lv >= 0 && !is_chapter) seen = 1;
    if (lv < 0) seen = 1;
    const char *tok = NULL;
    switch (field) {
      case F_UNLOCKED:
        if (k.kind == K_LV_LOCKED) tok = on ? "0" : "1";
        else if (k.kind == K_CH_LOCKED) tok = on ? "0" : (lv < 0 ? "1" : NULL);
        break;
      case F_COMPLETED:
        if (k.kind == K_LV_DONE || (k.kind == K_CH_DONE && lv < 0)) tok = on ? "1" : "0";
        else if (on && (k.kind == K_LV_LOCKED || k.kind == K_CH_LOCKED)) tok = "0";
        break;
      case F_VASES:
        if (k.kind == K_LV_VASE) tok = on ? "1" : "0";
        break;
    }
    if (!tok) continue;
    if (!token_kind_ok(f, i, T_I32)) continue;
    (*touched)++;
    const int r = set_token(f, i, tok);
    if (r < 0) return -2;
    changed += r;
  }
  return seen ? changed : -1;
}

/* Outfits: every "<name>_IsOutfitLocked" key, in the save's (= m_Outfits') order. */
static int outfit_list(const SFile *f, int *idx, int max) {
  int n = 0;
  static const char suf[] = "_IsOutfitLocked";
  for (int i = 0; i < f->n && n < max; i++) {
    const size_t l = f->p[i].ke - f->p[i].ks;
    if (l > sizeof suf - 1 && !memcmp(f->j + f->p[i].ke - (sizeof suf - 1), suf, sizeof suf - 1)) idx[n++] = i;
  }
  return n;
}
/* "Hitman" and "HitmanOutfitDesc" both name HitmanOutfitDesc_IsOutfitLocked. */
static void outfit_short(const SFile *f, int i, char *out, size_t n) {
  char full[128];
  key_of(f, i, full, sizeof full);
  char *s = strstr(full, "_IsOutfitLocked");
  if (s) *s = 0;
  const size_t l = strlen(full);
  if (l > 10 && !strcmp(full + l - 10, "OutfitDesc")) full[l - 10] = 0;
  snprintf(out, n, "%s", full);
}
static int outfit_matches(const SFile *f, int i, const char *name) {
  char s[128], full[160];
  outfit_short(f, i, s, sizeof s);
  snprintf(full, sizeof full, "%sOutfitDesc", s);
  return !strcasecmp(name, s) || !strcasecmp(name, full);
}

static int find_key(const SFile *f, const char *key) {
  for (int i = 0; i < f->n; i++) if (key_is(f, i, key)) return i;
  return -1;
}
/* Set key to tok, adding it if the file does not have it; with keep_existing
 * an existing value is left alone. Returns changes made (0/1), -1 on failure. */
static int ensure_value(SFile *f, SType t, const char *key, const char *tok, int keep_existing) {
  const int i = find_key(f, key);
  if (i >= 0) {
    if (keep_existing) return 0;
    if (!token_kind_ok(f, i, t)) return -1;
    return set_token(f, i, tok);
  }
  return append_pair(f, key, tok);
}
/* The comma-separated id list inside a quoted token: 1 if id is in it. */
static int csv_has(const char *tok, const char *id) {
  const size_t il = strlen(id);
  const char *p = tok + 1, *end = tok + strlen(tok) - 1;              /* inside the quotes */
  while (p < end) {
    const char *c = memchr(p, ',', (size_t)(end - p));
    const char *e = c ? c : end;
    if ((size_t)(e - p) == il && !memcmp(p, id, il)) return 1;
    p = e + 1;
  }
  return 0;
}
/* The quoted list with id added (on) or removed (off), order kept. */
static void csv_edit(const char *tok, const char *id, int on, char *out, size_t n) {
  const size_t il = strlen(id);
  const char *p = tok + 1, *end = tok + strlen(tok) - 1;
  size_t o = 0;
  out[o++] = '"';
  int first = 1;
  while (p < end) {
    const char *c = memchr(p, ',', (size_t)(end - p));
    const char *e = c ? c : end;
    const int is_id = (size_t)(e - p) == il && !memcmp(p, id, il);
    if (e > p && !(is_id && !on) && o + (size_t)(e - p) + 3 < n) {
      if (!first) out[o++] = ',';
      memcpy(out + o, p, (size_t)(e - p)); o += (size_t)(e - p);
      first = 0;
    }
    p = e + 1;
  }
  if (on && !csv_has(tok, id) && o + il + 3 < n) {
    if (!first) out[o++] = ',';
    memcpy(out + o, id, il); o += il;
  }
  out[o++] = '"';
  out[o] = 0;
}

/* hints = true: the Complete Walkthrough, as a shop-inventory entry.
 *
 * GameStructure.IsCompleteWalkThroughPurchased() is
 *   ArmoryShop.GetInventoryItem(s_CompleteWalkthroughId).Quantity > 0.
 * The inventory is built in ShopController.Initialize (ArmoryShop's static
 * constructor), before any store backend registers, by Persistor.Load:
 *   ids = "technology.shop.inventory.ids", split on ','; ids that are not in
 *   the shop's stock are dropped; for each, the item type comes from the STOCK
 *   entry (technology.shop.stock.<id>.type), then Item.Deserialize reads
 *   technology.shop.inventory.<id>.availQty -- a missing quantity drops the
 *   entry, anything else becomes its InventoryEntry.
 * Item.Serialize, which the game uses when it saves a purchase, writes that
 * quantity (Int32) and .type, .uri and .oskeymap (String) under the inventory
 * prefix, then Persistor.Save writes the id list. We write the same five
 * values. false sets the quantity to 0 and takes the id out of the list. */
#define HINT_SKU     "com.squareenix.laracroftgo.completewalkthrough"
#define INV_PREFIX   "technology.shop.inventory."
static int apply_hints(SFile *fs, const char *key, const char *val) {
  int on;
  if (!parse_bool(val, &on)) { se_log("%s = %s: use true or false -- skipped", key, val); return -1; }
  SFile *fi = &fs[T_I32], *fstr = &fs[T_STR];
  if (!fi->ok || !fstr->ok) {
    se_log("%s: SaveDataInt32.runtime and SaveDataString.runtime are both needed%s -- skipped", key,
           fi->present && fstr->present ? " and one could not be read" : " (play once, quit, then edit)");
    return -1;
  }
  const int stock = find_key(fstr, "technology.shop.stock.ids");
  char tok[1024];
  if (stock >= 0) token_of(fstr, stock, tok, sizeof tok);
  if (on && (stock < 0 || !token_kind_ok(fstr, stock, T_STR) || !csv_has(tok, HINT_SKU))) {
    se_log("%s: the shop's stock in the save does not list the Complete Walkthrough, so the game "
           "would drop it -- skipped", key);
    return -1;
  }
  const int ids = find_key(fstr, INV_PREFIX "ids");
  char list[1024] = "\"\"", nl[1100];
  if (ids >= 0) {
    if (!token_kind_ok(fstr, ids, T_STR)) { se_log("%s: " INV_PREFIX "ids is not text in the save -- skipped", key); return -1; }
    token_of(fstr, ids, list, sizeof list);
    if (fstr->p[ids].ve - fstr->p[ids].vs >= sizeof list - 1) { se_log("%s: the inventory list is too long -- skipped", key); return -1; }
  }
  csv_edit(list, HINT_SKU, on, nl, sizeof nl);
  int ch = 0, r;
  if (on) {
    if ((r = ensure_value(fi, T_I32, INV_PREFIX HINT_SKU ".availQty", "1", 0)) < 0) { goto bad; } else { ch += r; }
    if ((r = ensure_value(fstr, T_STR, INV_PREFIX HINT_SKU ".type", "\"Technology.Shop.NonConsumable\"", 1)) < 0) { goto bad; } else { ch += r; }
    if ((r = ensure_value(fstr, T_STR, INV_PREFIX HINT_SKU ".uri", "\"\"", 1)) < 0) { goto bad; } else { ch += r; }
    if ((r = ensure_value(fstr, T_STR, INV_PREFIX HINT_SKU ".oskeymap", "\"\"", 1)) < 0) { goto bad; } else { ch += r; }
    if ((r = ensure_value(fstr, T_STR, INV_PREFIX "ids", nl, 0)) < 0) { goto bad; } else { ch += r; }
  } else {
    const int q = find_key(fi, INV_PREFIX HINT_SKU ".availQty");
    if (q >= 0) { if (!token_kind_ok(fi, q, T_I32) || (r = set_token(fi, q, "0")) < 0) goto bad; ch += r; }
    if (ids >= 0) { if ((r = set_token(fstr, ids, nl)) < 0) goto bad; ch += r; }
  }
  se_log(on ? "%s = %s: the Complete Walkthrough is in the shop inventory%s"
            : "%s = %s: the Complete Walkthrough is out of the shop inventory%s",
         key, val, ch ? "" : " (already so)");
  return ch > 0;
bad:
  se_log("%s: a value in the save is not in the form the game writes, or out of memory -- skipped", key);
  return -1;
}

/* ======================================================================= */
/* save.txt                                                                */
/* ======================================================================= */
static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { se_log("could not write %s", path); return; }
  fputs(
"# save.txt -- Lara Croft GO save editing (lcgo_nx).\n"
"#\n"
"# Every line is commented out. Remove the '#' from one and give it a value; it\n"
"# is applied to the save at EVERY launch, for as long as the line stays\n"
"# uncommented. Comment it out again once the change has taken.\n"
"#\n"
"# The save is five files next to the game (SaveDataInt32.runtime and four\n"
"# others). They are encrypted and compressed, so editing them by hand breaks\n"
"# them. Editing here is safe: the port re-encodes them the way the game reads\n"
"# them, checks the result before writing, and keeps each untouched original\n"
"# once as <name>.runtime.orig.\n"
"#\n"
"# Only values your save already holds are changed, unless you use an add.\n"
"# line (at the end). Nothing is ever removed.\n"
"# What happened on the last launch is written to save.log. Every value in\n"
"# your save is listed at the end of this file, as the game last left it.\n"
"# Quit the game fully before editing: it saves as you play.\n"
"#\n"
"# true/false also accept 1/0, yes/no and on/off. A comment can follow a\n"
"# value after a space: chapter.all.unlocked = true   # like this\n"
"\n"
"# --- progress --------------------------------------------------------------\n"
"# Chapters and levels count from 0, exactly as the save names them. This\n"
"# version has 7 chapters: 0-4 are the main story, 5 and 6 the expansions.\n"
"#   chapter 0: levels 0-4      chapter 4: levels 0-2\n"
"#   chapter 1: levels 0-10     chapter 5: levels 0-10\n"
"#   chapter 2: levels 0-12     chapter 6: levels 0-10\n"
"#   chapter 3: levels 0-8\n"
"#\n"
"# unlocked  = true opens a chapter and every level in it (false locks them).\n"
"# completed = true marks them done, and unlocks them too, since a finished\n"
"#             level has to be reachable (false clears the done mark).\n"
"# vases     = true breaks every vase: all relic fragments and gems found\n"
"#             (false puts them back). This does NOT unlock the relic outfits:\n"
"#             the game hands those out the moment a relic is completed in\n"
"#             play. Use the outfit lines below for that.\n"
"#\n"
"# chapter.all does every chapter. A chapter.N line overrides it, and a\n"
"# level.N.M line overrides both, wherever they are in the file.\n"
"#chapter.all.unlocked = true\n"
"#chapter.all.completed = true\n"
"#chapter.all.vases = true\n"
"#chapter.1.unlocked = true\n"
"#level.2.5.completed = true\n"
"#level.2.5.vases = true\n"
"# Marks the main story as finished.\n"
"#game_finished = true\n"
"\n"
"# --- outfits ---------------------------------------------------------------\n"
"# outfit.<name>.unlocked = true unlocks an outfit, false locks it again;\n"
"# outfit.all.unlocked does every outfit (a line for one outfit overrides it).\n"
"# current_outfit picks what Lara wears, by name or by number in this list\n"
"# (the game's own order). An outfit has to be unlocked to be worn.\n"
"#    0 Hitman              7 Arctic\n"
"#    1 DeusEx              8 Wetsuit\n"
"#    2 JustCause           9 Catsuit\n"
"#    3 Classic            10 Gold\n"
"#    4 ExpansionOutfit1   11 ExpansionOutfit2\n"
"#    5 Tibet              12 Spirit\n"
"#    6 Desert\n"
"#outfit.all.unlocked = true\n"
"#outfit.Tibet.unlocked = true\n"
"#current_outfit = Classic\n"
"\n"
"# --- hints -----------------------------------------------------------------\n"
"# hints = true gives you the Complete Walkthrough, the hint pack the game\n"
"# sells: it goes into the shop inventory in the save the way the game records\n"
"# a purchase. false takes it out again.\n"
"#hints = true\n"
"\n"
"# --- anything else ---------------------------------------------------------\n"
"# prop.<exact key> = value sets any value already in the save, verbatim. The\n"
"# key is looked up in all five files, and the value has to suit the file that\n"
"# holds it (a whole number, a number like 1.5, true/false, or one line of\n"
"# plain text; \"\" for empty text). A key the save does not have is reported\n"
"# in save.log and skipped; add. below adds keys. The list at the end of this\n"
"# file has a ready-made prop. line for every value.\n"
"#prop.Chapter_1_Level_3_HighestCheckpoint = 0\n"
"\n"
"# --- adding values ---------------------------------------------------------\n"
"# add.<type>.<key> = value puts a value in the save even if the game has not\n"
"# written it yet; if it is already there it is simply set, like prop. <type>\n"
"# picks the file it goes into: int, long, bool, float or string. A key that\n"
"# another file already holds under a different type is refused.\n"
"# add lines are applied first, so every other line can change what they add.\n"
"# The game only reads the keys it knows, so a misspelt key just sits in the\n"
"# save unused; check save.log. Commenting the line out again does NOT take the\n"
"# key back out. Restoring the .runtime.orig files does, but they are the save\n"
"# from before your first edit, so that also undoes all play since.\n"
"#add.int.Your_Key_Name = 1\n", f);
  fclose(f);
  se_log("wrote %s (every setting commented out)", path);
}

static char *trim(char *p) {
  while (isspace((unsigned char)*p)) p++;
  char *e = p + strlen(p);
  while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
  return p;
}

/* Which pass a setting belongs to (general before specific), or -1. */
enum { P_ADD, P_CH_ALL, P_CH, P_LV, P_GAME, P_OUT_ALL, P_OUT, P_CUR, P_HINTS, P_PROP, P_N };
static int setting_pass(const char *k) {
  if (!strncasecmp(k, "add.", 4))          return P_ADD;
  if (!strncasecmp(k, "chapter.all.", 12)) return P_CH_ALL;
  if (!strncasecmp(k, "chapter.", 8))      return P_CH;
  if (!strncasecmp(k, "level.", 6))        return P_LV;
  if (!strcasecmp(k, "game_finished"))     return P_GAME;
  if (!strncasecmp(k, "outfit.all.", 11))  return P_OUT_ALL;
  if (!strncasecmp(k, "outfit.", 7))       return P_OUT;
  if (!strcasecmp(k, "current_outfit"))    return P_CUR;
  if (!strcasecmp(k, "hints"))             return P_HINTS;
  if (!strncmp(k, "prop.", 5))             return P_PROP;
  return -1;
}

static const char *need_i32(SFile *fs, const char *key) {
  if (!fs[T_I32].present) return "SaveDataInt32.runtime is not there yet (play once, quit, then edit)";
  if (!fs[T_I32].ok)      return "SaveDataInt32.runtime could not be read (see above)";
  (void)key;
  return NULL;
}

/* Apply one setting. 1 changed, 0 already so, -1 skipped. */
static int apply_setting(SFile *fs, int pass, const char *key, const char *val) {
  SFile *f32 = &fs[T_I32];
  const char *miss;

  if (pass == P_CH_ALL || pass == P_CH || pass == P_LV) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", key);
    char *parts[5] = { 0 };
    int np = 0;
    for (char *s = buf, *d; np < 5; s = d + 1) { parts[np++] = s; if (!(d = strchr(s, '.'))) break; *d = 0; }
    const int want = pass == P_LV ? 4 : 3;
    int ch = -1, lv = -1, field, on;
    if (np != want || (field = progress_field(parts[want - 1])) < 0) {
      se_log("%s: expected %s.<unlocked|completed|vases> -- skipped", key,
             pass == P_LV ? "level.<chapter>.<level>" : "chapter.<number|all>");
      return -1;
    }
    if (pass != P_CH_ALL) {
      char *e;
      ch = (int)strtol(parts[1], &e, 10);
      if (!*parts[1] || *e || ch < 0 || ch > 999) { se_log("%s: '%s' is not a chapter number -- skipped", key, parts[1]); return -1; }
      if (pass == P_LV) {
        lv = (int)strtol(parts[2], &e, 10);
        if (!*parts[2] || *e || lv < 0 || lv > 999) { se_log("%s: '%s' is not a level number -- skipped", key, parts[2]); return -1; }
      }
    }
    if (!parse_bool(val, &on)) { se_log("%s = %s: use true or false -- skipped", key, val); return -1; }
    if ((miss = need_i32(fs, key))) { se_log("%s: %s -- skipped", key, miss); return -1; }
    int touched = 0;
    const int r = apply_progress(f32, ch, lv, field, on, &touched);
    if (r == -2) { se_log("%s: out of memory -- skipped", key); return -1; }
    if (r < 0) {
      if (lv >= 0) se_log("%s: the save has no chapter %d level %d -- skipped", key, ch, lv);
      else         se_log("%s: the save has no chapter %d -- skipped", key, ch);
      return -1;
    }
    se_log("%s = %s: %d value(s), %d changed", key, val, touched, r);
    return r > 0;
  }

  if (pass == P_GAME) {
    int on;
    if (!parse_bool(val, &on)) { se_log("%s = %s: use true or false -- skipped", key, val); return -1; }
    if ((miss = need_i32(fs, key))) { se_log("%s: %s -- skipped", key, miss); return -1; }
    for (int i = 0; i < f32->n; i++) {
      if (!key_is(f32, i, "IsGameFinished") || !token_kind_ok(f32, i, T_I32)) continue;
      const int r = set_token(f32, i, on ? "1" : "0");
      if (r < 0) { se_log("%s: out of memory -- skipped", key); return -1; }
      se_log("%s = %s%s", key, val, r ? "" : " (already so)");
      return r;
    }
    se_log("%s: the save has no IsGameFinished yet -- skipped", key);
    return -1;
  }

  if (pass == P_OUT_ALL || pass == P_OUT) {
    const char *name = key + 7;                            /* after "outfit." */
    const char *dot = strrchr(name, '.');
    int on;
    if (!dot || dot == name || strcasecmp(dot + 1, "unlocked")) {
      se_log("%s: expected outfit.<name|all>.unlocked -- skipped", key); return -1;
    }
    if (!parse_bool(val, &on)) { se_log("%s = %s: use true or false -- skipped", key, val); return -1; }
    if ((miss = need_i32(fs, key))) { se_log("%s: %s -- skipped", key, miss); return -1; }
    char nm[96];
    snprintf(nm, sizeof nm, "%.*s", (int)(dot - name), name);
    int idx[64];
    const int n = outfit_list(f32, idx, 64);
    int hits = 0, changed = 0;
    for (int o = 0; o < n; o++) {
      if (pass == P_OUT && !outfit_matches(f32, idx[o], nm)) continue;
      if (!token_kind_ok(f32, idx[o], T_I32)) continue;
      hits++;
      const int r = set_token(f32, idx[o], on ? "0" : "1");
      if (r < 0) { se_log("%s: out of memory -- skipped", key); return -1; }
      changed += r;
    }
    if (!hits) {
      se_log("%s: no outfit called '%s' in the save (the names are listed in save.txt) -- skipped", key, nm);
      return -1;
    }
    se_log("%s = %s: %d outfit(s), %d changed", key, val, hits, changed);
    return changed > 0;
  }

  if (pass == P_CUR) {
    if ((miss = need_i32(fs, key))) { se_log("%s: %s -- skipped", key, miss); return -1; }
    int idx[64];
    const int n = outfit_list(f32, idx, 64);
    int pick = -1;
    char *e;
    const long num = strtol(val, &e, 10);
    if (*val && !*e) pick = (num >= 0 && num < n) ? (int)num : -2;
    else for (int o = 0; o < n; o++) if (outfit_matches(f32, idx[o], val)) { pick = o; break; }
    if (pick < 0) {
      se_log("%s = %s: %s -- skipped", key, val,
             pick == -2 ? "no outfit has that number" : "no outfit by that name (see the list in save.txt)");
      return -1;
    }
    char tok[16], nm[96];
    token_of(f32, idx[pick], tok, sizeof tok);
    outfit_short(f32, idx[pick], nm, sizeof nm);
    if (strcmp(tok, "0")) {
      se_log("%s = %s: %s is locked -- add outfit.%s.unlocked = true -- skipped", key, val, nm, nm);
      return -1;
    }
    for (int i = 0; i < f32->n; i++) {
      if (!key_is(f32, i, "CurrentOutfitIndex") || !token_kind_ok(f32, i, T_I32)) continue;
      char num_s[16];
      snprintf(num_s, sizeof num_s, "%d", pick);
      const int r = set_token(f32, i, num_s);
      if (r < 0) { se_log("%s: out of memory -- skipped", key); return -1; }
      se_log("%s = %s (%s, number %d)%s", key, val, nm, pick, r ? "" : " (already so)");
      return r;
    }
    se_log("%s: the save has no CurrentOutfitIndex yet -- skipped", key);
    return -1;
  }

  if (pass == P_HINTS) return apply_hints(fs, key, val);

  if (pass == P_ADD) {
    const char *tn = key + 4, *dot = strchr(tn, '.');
    const int t = dot ? type_by_name(tn, (size_t)(dot - tn)) : -1;
    if (t < 0 || !dot[1]) {
      se_log("%s: expected add.<int|long|bool|float|string>.<key> -- skipped", key); return -1;
    }
    const char *nk = dot + 1;
    if (!is_plain_key(nk)) { se_log("%s: a key has to be plain text without quotes or backslashes -- skipped", key); return -1; }
    SFile *f = &fs[t];
    if (!f->present) { se_log("%s: %s.runtime is not there yet (play once, quit, then edit) -- skipped", key, k_name[t]); return -1; }
    if (!f->ok)      { se_log("%s: %s.runtime could not be read (see above) -- skipped", key, k_name[t]); return -1; }
    for (int o = 0; o < T_N; o++) {
      if (o == t || !fs[o].ok) continue;
      for (int i = 0; i < fs[o].n; i++)
        if (key_is(&fs[o], i, nk)) {
          se_log("%s: '%s' is already in %s.runtime, as a different type -- skipped", key, nk, k_name[o]);
          return -1;
        }
    }
    char tok[1024];
    const char *why = "?";
    if (!make_token((SType)t, val, tok, sizeof tok, &why)) { se_log("%s = %s: the value has to be %s -- skipped", key, val, why); return -1; }
    int changed = 0, found = 0;
    for (int i = 0; i < f->n; i++) {
      if (!key_is(f, i, nk)) continue;
      found = 1;
      if (!token_kind_ok(f, i, (SType)t)) { se_log("%s: the value in the save is not in the form this file uses -- skipped", key); return -1; }
      char old[64];
      token_of(f, i, old, sizeof old);
      const int r = set_token(f, i, tok);
      if (r < 0) { se_log("%s: out of memory -- skipped", key); return -1; }
      if (r) se_log("%s: already in the save, %s -> %s", key, old, tok);
      changed += r;
    }
    if (found) {
      if (!changed) se_log("%s = %s: already in the save with that value", key, val);
      return changed > 0;
    }
    if (append_pair(f, nk, tok) < 0) { se_log("%s: out of memory -- skipped", key); return -1; }
    se_log("%s = %s: added to %s.runtime", key, val, k_name[t]);
    return 1;
  }

  if (pass == P_PROP) {
    const char *pk = key + 5;
    int hit_file = -1, hits = 0;
    for (int t = 0; t < T_N; t++) {
      if (!fs[t].ok) continue;
      for (int i = 0; i < fs[t].n; i++) if (key_is(&fs[t], i, pk)) { if (hit_file != t) hits++; hit_file = t; break; }
    }
    if (!hits) { se_log("%s: no value called '%s' in the save -- skipped (add.<type>.%s adds one)", key, pk, pk); return -1; }
    if (hits > 1) { se_log("%s: '%s' is in more than one save file -- skipped", key, pk); return -1; }
    SFile *f = &fs[hit_file];
    char tok[1024];
    const char *why = "?";
    if (!make_token((SType)hit_file, val, tok, sizeof tok, &why)) {
      se_log("%s = %s: %s.runtime holds %s here -- skipped", key, val, k_name[hit_file], why);
      return -1;
    }
    int changed = 0;
    for (int i = 0; i < f->n; i++) {
      if (!key_is(f, i, pk)) continue;
      if (!token_kind_ok(f, i, (SType)hit_file)) {
        se_log("%s: the value in the save is not in the form this file uses -- skipped", key); return -1;
      }
      char old[64];
      token_of(f, i, old, sizeof old);
      const int r = set_token(f, i, tok);
      if (r < 0) { se_log("%s: out of memory -- skipped", key); return -1; }
      if (r) se_log("%s: %s -> %s", key, old, tok);
      changed += r;
    }
    return changed > 0;
  }
  return -1;
}

/* ======================================================================= */
/* the list of everything in the save, at the end of save.txt              */
/* ======================================================================= */
#define LIST_START "# === everything in your save"
#define LIST_END   "# === end of the list"
#define LIST_MAX_VALUE 120        /* longer text is named, not listed */
int g_list_writes;                /* for the tests: how often save.txt was rewritten */

typedef struct { char *b; size_t n, cap; int oom; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
  if (b->oom) return;
  if (b->n + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 16384;
    while (nc < b->n + n + 1) nc *= 2;
    char *nb = realloc(b->b, nc);
    if (!nb) { b->oom = 1; return; }
    b->b = nb; b->cap = nc;
  }
  memcpy(b->b + b->n, s, n); b->n += n; b->b[b->n] = 0;
}
static void buf_line(Buf *b, const char *nl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void buf_line(Buf *b, const char *nl, const char *fmt, ...) {
  char line[1400];
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(line, sizeof line, fmt, va);
  va_end(va);
  if (n < 0) return;
  if (n >= (int)sizeof line) n = (int)sizeof line - 1;
  buf_add(b, line, (size_t)n);
  buf_add(b, nl, strlen(nl));
}
/* Start of the line that begins with lit, at or after p; NULL if none. */
static char *line_starting(char *text, char *p, const char *lit) {
  const size_t l = strlen(lit);
  for (char *q = p; q && *q; ) {
    if (!strncmp(q, lit, l) && (q == text || q[-1] == '\n')) return q;
    q = strchr(q, '\n');
    if (q) q++;
  }
  return NULL;
}
/* A key that can go on a prop. line and come back unchanged. */
static int listable_key(const char *k) {
  const size_t l = strlen(k);
  return is_plain_key(k) && !strchr(k, '=') && !strstr(k, " #") && k[0] != ' ' && k[l - 1] != ' ';
}
/* The value as it would be typed back in, or 0 with a reason. */
static int listable_value(const SFile *f, int i, SType t, char *out, size_t n, const char **why) {
  const size_t vl = f->p[i].ve - f->p[i].vs;
  const char *v = f->j + f->p[i].vs;
  if (t != T_STR) {
    if (vl >= n) { *why = "too long"; return 0; }
    memcpy(out, v, vl); out[vl] = 0;
    return 1;
  }
  const size_t cl = vl - 2;                                  /* inside the quotes */
  if (cl == 0) { snprintf(out, n, "\"\""); return 1; }
  if (cl > LIST_MAX_VALUE) { *why = "long"; return 0; }
  memcpy(out, v + 1, cl); out[cl] = 0;
  if (!is_plain_text(out) || strstr(out, " #") || strstr(out, "\t#") || out[0] == ' ' || out[cl - 1] == ' ' ||
      !strcmp(out, "\"\"")) {
    *why = "contains characters save.txt cannot carry"; return 0;
  }
  return 1;
}
/* The prop. key of a live (uncommented) line, or NULL. */
static int live_prop_key(const char *line, size_t len, char *key, size_t n) {
  while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
  if (len < 6 || strncmp(line, "prop.", 5)) return 0;
  const char *eq = memchr(line, '=', len);
  if (!eq) return 0;
  const char *ks = line + 5, *ke = eq;
  while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
  if ((size_t)(ke - ks) >= n || ke == ks) return 0;
  memcpy(key, ks, (size_t)(ke - ks)); key[ke - ks] = 0;
  return 1;
}

/* Rewrite the list at the end of save.txt from fs. Everything above it stays
 * byte for byte; lines you uncommented inside it stay as you wrote them (a
 * prop. line in its key's place, anything else at the end of the list); line
 * endings follow the file. Written only if the text actually changes. */
static void list_refresh(const char *path, const SFile *fs) {
  size_t n = 0;
  char *t = read_file(path, &n);
  if (!t) return;
  const char *nl = strstr(t, "\r\n") ? "\r\n" : "\n";
  char *s = line_starting(t, t, LIST_START);
  char *e = s ? line_starting(t, s, LIST_END) : NULL;
  char *after = e ? (strchr(e, '\n') ? strchr(e, '\n') + 1 : e + strlen(e)) : t + n;

  /* the live lines of the old list */
  enum { MAXL = 4096 };
  static struct { const char *p; size_t l; int used; char key[256]; int is_prop; } live[MAXL];
  int nlive = 0;
  if (s) {
    const char *stop = e ? e : t + n;
    for (const char *q = s; q < stop && nlive < MAXL; ) {
      const char *qe = memchr(q, '\n', (size_t)(stop - q));
      const char *lend = qe ? qe : stop;
      size_t l = (size_t)(lend - q);
      if (l && q[l - 1] == '\r') l--;
      const char *c = q;
      while (c < q + l && (*c == ' ' || *c == '\t')) c++;
      if (c < q + l && *c != '#') {
        live[nlive].p = q; live[nlive].l = l; live[nlive].used = 0;
        live[nlive].is_prop = live_prop_key(q, l, live[nlive].key, sizeof live[nlive].key);
        nlive++;
      }
      q = qe ? qe + 1 : stop;
    }
  }

  Buf b = { 0 };
  if (s) buf_add(&b, t, (size_t)(s - t));
  else {
    buf_add(&b, t, n);
    if (n && t[n - 1] != '\n') buf_add(&b, nl, strlen(nl));
    buf_add(&b, nl, strlen(nl));
  }
  buf_line(&b, nl, "%s ===============================================", LIST_START);
  buf_line(&b, nl, "# Every value in the five save files, as they were when the game last");
  buf_line(&b, nl, "# started (after the lines above were applied). This list is rewritten at");
  buf_line(&b, nl, "# every launch. To change a value, remove its '#' and edit it: it works like");
  buf_line(&b, nl, "# any prop. line, and the line stays as you wrote it when the list is");
  buf_line(&b, nl, "# rewritten. Anything else you write between the two === lines is dropped,");
  buf_line(&b, nl, "# except other uncommented lines, which are kept at the end of the list.");
  static const char *const what[T_N] = { "whole numbers", "true/false", "whole numbers (64-bit)",
                                         "numbers", "text (\"\" is empty)" };
  for (int ty = 0; ty < T_N; ty++) {
    const SFile *f = &fs[ty];
    if (!f->ok) continue;
    buf_line(&b, nl, "#");
    buf_line(&b, nl, "# --- %s.runtime: %d value%s, %s", k_name[ty], f->n, f->n == 1 ? "" : "s", what[ty]);
    for (int i = 0; i < f->n; i++) {
      char key[256], v[LIST_MAX_VALUE + 8];
      const size_t kl = f->p[i].ke - f->p[i].ks;
      if (kl >= sizeof key) { buf_line(&b, nl, "# (a key of %zu characters, not listed)", kl); continue; }
      memcpy(key, f->j + f->p[i].ks, kl); key[kl] = 0;
      int k_live = -1;
      for (int L = 0; L < nlive; L++) if (live[L].is_prop && !live[L].used && !strcmp(live[L].key, key)) { k_live = L; break; }
      if (k_live >= 0) {
        buf_add(&b, live[k_live].p, live[k_live].l); buf_add(&b, nl, strlen(nl));
        live[k_live].used = 1;
        continue;
      }
      int elsewhere = 0;
      for (int o = 0; o < T_N; o++) if (o != ty && fs[o].ok && find_key(&fs[o], key) >= 0) elsewhere = 1;
      const char *why = "";
      if (!listable_key(key) || elsewhere) {
        char tok[64]; token_of(f, i, tok, sizeof tok);
        buf_line(&b, nl, "# %s = %s   (can't be set from here: %s)", key, tok,
                 elsewhere ? "the same key is in another file" : "the key has characters save.txt cannot carry");
      } else if (listable_value(f, i, (SType)ty, v, sizeof v, &why)) {
        buf_line(&b, nl, "#prop.%s = %s", key, v);
      } else if (!strcmp(why, "long")) {
        buf_line(&b, nl, "# %s: text, %zu characters (too long to list)", key, f->p[i].ve - f->p[i].vs - 2);
      } else {
        buf_line(&b, nl, "# %s: %s", key, why);
      }
    }
  }
  int other = 0;
  for (int L = 0; L < nlive; L++) {
    if (live[L].used) continue;
    if (!other++) { buf_line(&b, nl, "#"); buf_line(&b, nl, "# --- your own lines (kept from the list)"); }
    buf_add(&b, live[L].p, live[L].l); buf_add(&b, nl, strlen(nl));
  }
  buf_line(&b, nl, "%s =================================================", LIST_END);
  if (e) buf_add(&b, after, (size_t)(t + n - after));

  if (!b.oom && (b.n != n || memcmp(b.b, t, n))) {
    char tmp[560];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int ok = write_file(tmp, b.b, b.n);
    if (ok) { remove(path); ok = rename(tmp, path) == 0 || write_file(path, b.b, b.n); remove(tmp); }
    if (ok) g_list_writes++;
    else se_log("could not update the list of values in save.txt");
  }
  free(b.b);
  free(t);
}

/* ======================================================================= */
/* entry                                                                   */
/* ======================================================================= */
static void report_write(const char *dir) {
  char p[512];
  snprintf(p, sizeof p, "%s/save.log", dir);
  if (!write_file(p, g_rep, g_rep_n)) {
#ifdef __SWITCH__
    debugPrintf("[saveedit] could not write %s\n", p);
#endif
  }
}

int lcgo_saveedit_run_dir(const char *dir) {
  g_rep_n = 0; g_rep[0] = 0; g_rep_full = 0;
  char ep[512], lp[512];
  snprintf(ep, sizeof ep, "%s/save.txt", dir);
  snprintf(lp, sizeof lp, "%s/save.log", dir);

  size_t elen = 0;
  char *edit = read_file(ep, &elen);
  if (!edit) {
    write_template(ep);
    edit = read_file(ep, &elen);          /* all comments: no settings, but the list still goes in */
  }

  /* 1. the uncommented settings */
  static Setting set[SE_MAX_SET];
  int ns = 0, dropped = 0;
  for (char *ln = edit ? strtok(edit, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
    char *t = trim(ln);
    if (!*t || *t == '#') continue;
    char *eq = strchr(t, '=');
    if (!eq) { se_log("ignoring '%s' (no '=')", t); continue; }
    *eq = 0;
    /* a comment after the value: '#' with a space or tab before it */
    for (char *h = eq + 1; *h; h++) if (*h == '#' && (h[-1] == ' ' || h[-1] == '\t')) { *h = 0; break; }
    char *k = trim(t), *v = trim(eq + 1);
    if (!*k || !*v) { se_log("ignoring '%s' (no value)", k); continue; }
    if (setting_pass(k) < 0) { se_log("unknown setting '%s' -- skipped", k); continue; }
    if (ns < SE_MAX_SET) { set[ns].k = k; set[ns].v = v; ns++; }
    else dropped++;
  }
  if (dropped) se_log("more than %d settings -- the last %d ignored", SE_MAX_SET, dropped);

  /* 2. the save */
  SFile fs[T_N];
  memset(fs, 0, sizeof fs);
  int any = 0, failed = 0, n_changed = 0, list_ok = 1;
  for (int t = 0; t < T_N; t++) { sfile_load(&fs[t], dir, (SType)t); any |= fs[t].present; }
  for (int t = 0; t < T_N; t++) failed |= fs[t].present && !fs[t].ok;   /* reported, never written */
  if (!any) {
    if (ns) se_log("no save yet (no SaveData*.runtime in %s) -- play once, quit, then edit", dir);
    goto done;
  }

  if (ns) {
    for (int pass = 0; pass < P_N; pass++)
      for (int i = 0; i < ns; i++)
        if (setting_pass(set[i].k) == pass) apply_setting(fs, pass, set[i].k, set[i].v);

    /* A file counts as changed only if its text now differs: lines that undo
     * each other (add then chapter.all, say) must not rewrite it every launch. */
    for (int t = 0; t < T_N; t++) { fs[t].changed = sfile_differs(&fs[t]); n_changed += fs[t].changed; }
    if (!n_changed) se_log("the save already matches save.txt -- nothing written");
  }

  if (n_changed) {
    /* 3. encode every changed file and prove it before writing any of them */
    int vfail = 0;
    for (int t = 0; t < T_N; t++) {
      SFile *f = &fs[t];
      if (!f->changed) continue;
      f->out = lcgo_save_encode(f->j, f->jn, f->props, &f->out_n);
      int ok = f->out != NULL;
      if (ok) {
        char *back = NULL; size_t bn = 0; unsigned char bp[5]; char err[160];
        Pair *bpairs = NULL; const char *why = "?";
        ok = !lcgo_save_decode(f->out, f->out_n, &back, &bn, bp, err, sizeof err)
          && bn == f->jn && !memcmp(back, f->j, bn) && !memcmp(bp, f->props, 5);
        int bc = ok ? json_scan(back, bn, &bpairs, &why) : -1;
        ok = ok && bc == f->n;
        for (int i = 0; ok && i < f->n; i++)
          ok = bpairs[i].ke - bpairs[i].ks == f->p[i].ke - f->p[i].ks &&
               !memcmp(back + bpairs[i].ks, f->j + f->p[i].ks, f->p[i].ke - f->p[i].ks);
        free(back); free(bpairs);
      }
      if (!ok) { se_log("%s.runtime: the re-encoded file did not read back identically", k_name[t]); vfail = 1; }
    }
    if (vfail) {
      se_log("verification FAILED -- nothing written, the save is as it was");
      failed = 1; list_ok = 0;
      goto done;
    }

    /* 4-5. originals once, then temp + rename */
    for (int t = 0; t < T_N; t++) {
      SFile *f = &fs[t];
      if (!f->changed) continue;
      char orig[560], tmp[560];
      snprintf(orig, sizeof orig, "%s.orig", f->path);
      snprintf(tmp, sizeof tmp, "%s.tmp", f->path);
      if (!file_exists(orig)) {
        if (!write_file(orig, f->raw, f->raw_n)) {
          se_log("%s.runtime: could not keep the original as %s.runtime.orig -- not written", k_name[t], k_name[t]);
          failed = 1; list_ok = 0;
          continue;
        }
        se_log("%s.runtime: original kept as %s.runtime.orig", k_name[t], k_name[t]);
      }
      int ok = write_file(tmp, f->out, f->out_n);
      if (ok) {
        remove(f->path);
        ok = rename(tmp, f->path) == 0 || write_file(f->path, f->out, f->out_n);
        remove(tmp);
      }
      se_log(ok ? "%s.runtime: written" : "%s.runtime: WRITE FAILED", k_name[t]);
      if (!ok) { failed = 1; list_ok = 0; }
    }
  }

  /* 6. the list: only from a save that is fully readable and on the card as shown */
  if (failed) list_ok = 0;
  if (list_ok) list_refresh(ep, fs);
  else se_log("the list of values in save.txt was left as it was");

done:
  if (g_rep_n) report_write(dir);
  else if (file_exists(lp)) remove(lp);      /* nothing to report: no stale log */
  for (int t = 0; t < T_N; t++) sfile_free(&fs[t]);
  free(edit);
#ifdef __SWITCH__
  fsdevCommitDevice("sdmc");
#endif
  if (failed) return -1;
  return n_changed ? 1 : 0;
}

#ifdef __SWITCH__
void lcgo_saveedit_run(void) {
#if LCGO_SAVE_EDIT
  lcgo_saveedit_run_dir(GAME_HOME);
#endif
}
#endif

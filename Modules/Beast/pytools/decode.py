"""
Decoder for the Beast 2015 -lk key, transcribed from the Hex-Rays output of thebeast.exe
(sub_521C40 = decoder, sub_521C00 = 4-char -> byte, sub_521310 = type/expiry judgement).
READ-ONLY: it explains a key. It does not create keys.
"""
def _val(ch):                      # c<=64 -> c-48 else c-55 (the exe does this on a signed char)
    c = ord(ch)
    return (c - 48) if c <= 64 else (c - 55)

def decode(key):
    r = {"ok": False}
    if len(key) < 40:
        r["error"] = f"too short ({len(key)} chars, needs exactly 40)"; return r
    if len(key) > 40:
        r["error"] = f"too long ({len(key)} chars, needs exactly 40)"; return r
    if any(not (48 <= ord(c) <= 90) for c in key):
        r["error"] = "chars outside '0'..'Z' (ASCII 48..90)"; return r
    mask = (17 * (_val(key[0]) & 0xF)) & 0xFF
    pos = 0
    def take():
        nonlocal pos
        b = 0
        for i in range(0, 8, 2):
            b += (_val(key[pos]) & 3) << i
            pos += 1
        return (b & 0xFF) ^ mask
    b0 = take(); b1 = take()
    v9 = (b1 << 8) + b0
    r["month"] = (v9 >> 7) & 0xF
    r["year"] = (v9 & 0x7F) + 2000
    r["day"] = (v9 >> 11) & 0x1F
    r["field5"] = take()           # this[5]
    r["field4"] = take()           # this[4]  (judged as 6 or 12 later)
    f3 = take(); f3 += take() << 8; f3 += take() << 16; f3 += take() << 24
    r["field3"] = f3
    s = sum(ord(c) for c in key[:36]) & 0xFF
    chk = 0
    for i, c in enumerate(key[36:40]):
        chk += (_val(c) & 3) << (2 * i)
    r["sum36"] = s
    r["checksum_byte"] = chk
    r["checksum_ok"] = (s == ((chk & 0xFF) ^ mask))
    r["ok"] = r["checksum_ok"]
    return r

if __name__ == "__main__":
    import sys, json
    k = sys.argv[1] if len(sys.argv) > 1 else "0" * 40
    print(json.dumps(decode(k), indent=1))

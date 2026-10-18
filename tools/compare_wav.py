import struct, sys, array

def read_wav(path):
    raw = open(path, "rb").read()
    assert raw[:4] in (b"RIFF", b"RF64"), raw[:4]
    position = 12
    fmt = None
    while position + 8 <= len(raw):
        chunk = raw[position:position+4]
        size = struct.unpack("<I", raw[position+4:position+8])[0]
        body = raw[position+8:position+8+size]
        if chunk == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data":
            return fmt, (body if size != 0xFFFFFFFF else raw[position+8:])
        position += 8 + size + (size & 1)
    raise SystemExit("no data chunk in " + path)

fmt_a, data_a = read_wav(sys.argv[1])
fmt_b, data_b = read_wav(sys.argv[2])
print("A", fmt_a, "bytes", len(data_a))
print("B", fmt_b, "bytes", len(data_b))
count = min(len(data_a), len(data_b))
if fmt_a[1:4] != fmt_b[1:4] or count == 0:
    print("FORMAT MISMATCH")
    sys.exit(1)
if data_a[:count] == data_b[:count]:
    print("BIT-EXACT over", count, "bytes")
else:
    left = array.array("f"); left.frombytes(data_a[:count])
    right = array.array("f"); right.frombytes(data_b[:count])
    if fmt_a[3] == 32:
        diff = max(abs(x - y) for x, y in zip(left, right))
    else:
        diff = max(abs(x - y) for x, y in zip(data_a[:count], data_b[:count]))
    different = sum(1 for x, y in zip(data_a[:count], data_b[:count]) if x != y)
    print("DIFFERENT", different, "of", count, "bytes, max |diff|", diff)
    sys.exit(2)

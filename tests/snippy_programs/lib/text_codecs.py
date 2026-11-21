# UTF-16, UTF-32 and utf-8-sig, written and read, and their errors (the Snippy plan, part 5.3).
for enc in ["utf-16", "utf-16-le", "utf-16-be", "utf-32", "utf-32-le", "utf-32-be", "utf-8-sig"]:
    b = "h€\U0001f600".encode(enc)
    print(enc, b, b.decode(enc), len(b))
print(b"\xff".decode("utf-16-le", "replace"), b"\x00\xd8\x41\x00".decode("utf-16-le", "replace"))
for bad in [b"\xff", b"\x00\xdc"]:
    try:
        bad.decode("utf-16-le")
    except UnicodeDecodeError as e:
        print(e)

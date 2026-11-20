def slug(text: str) -> str:
    out = []
    dash = False
    for ch in text.strip().lower():
        if ch.isascii() and ch.isalnum():
            out.append(ch)
            dash = False
        elif not dash and out:
            out.append("-")
            dash = True
    return "".join(out).strip("-")

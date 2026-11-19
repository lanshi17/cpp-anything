"""A CLI that notifies over HTTP and email: the side-effect fixture."""
import email.message
import json
import os
import smtplib
import sys
import urllib.request


def notify() -> int:
    url = os.environ.get("WEBHOOK_URL", "")
    if not url:
        print("WEBHOOK_URL is not set", file=sys.stderr)
        return 2
    request = urllib.request.Request(
        url,
        data=json.dumps({"event": "ping"}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        print(response.status)
    return 0


def mail() -> int:
    host = os.environ.get("SMTP_HOST", "localhost")
    port = int(os.environ.get("SMTP_PORT", "25"))
    message = email.message.EmailMessage()
    message["From"] = "app@example.test"
    message["To"] = "user@example.test"
    message["Subject"] = "welcome"
    message.set_content("hello from the fixture")
    with smtplib.SMTP(host, port, timeout=5) as smtp:
        smtp.send_message(message)
    print("sent")
    return 0


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else ""
    if command == "notify":
        return notify()
    if command == "mail":
        return mail()
    if command == "save":
        with open("db.txt", "a", encoding="utf-8") as handle:
            handle.write(sys.argv[2] if len(sys.argv) > 2 else "row\n")
        print("saved")
        return 0
    print("usage: effects notify | mail | save <row>", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

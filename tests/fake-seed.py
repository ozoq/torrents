import socket
import sys

def read_line(conn):
    data = b""
    while not data.endswith(b"\n"):
        chunk = conn.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode("utf-8", errors="replace")

def main():
    host = "0.0.0.0"
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 10009

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(16)

    while True:
        conn, _ = s.accept()
        try:
            _ = read_line(conn)
            conn.sendall(b"DATA 999999\n")
            conn.sendall(b"x" * 10)
        finally:
            conn.close()

if __name__ == "__main__":
    main()

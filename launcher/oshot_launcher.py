#!/bin/env python3
from time import sleep
import threading
import pystray
import socket
import pyclip
import subprocess
import io
import struct
import pyperclipimg as pci
from PIL import Image, ImageDraw

OSHOT = 'oshot'
HOST = '127.0.0.1'
PORT = 6015

def create_image():
    # Simple icon for now
    image = Image.new('RGB', (64, 64), color='blue')
    dc = ImageDraw.Draw(image)
    dc.rectangle([16, 16, 48, 48], fill='white')
    return image

def setup(icon):
    icon.visible = True

def recv_exact(conn, size):
    buf = b""
    while len(buf) < size:
        chunk = conn.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("Socket closed")
        buf += chunk
    return buf

def listen_socket():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen()
        print(f"Listening on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            print(f"Connection from {addr}")
            threading.Thread(target=handle_client, args=(conn,), daemon=True).start()

def handle_client(conn):
    with conn:
        while True:
            try:
                type_byte = recv_exact(conn, 1)
                size_bytes = recv_exact(conn, 4)
                size = struct.unpack("!I", size_bytes)[0]
                payload = recv_exact(conn, size)

                if type_byte == b'T':
                    pyclip.copy(payload.decode('utf-8'))
                    icon.notify("Copied text!")
                elif type_byte == b'I':
                    image = Image.open(io.BytesIO(payload))
                    image.load()
                    pci.copy(image)
                    icon.notify("Copied image!")
            except ConnectionError:
                print("Client disconnected")
                break

def launch():
    subprocess.run([OSHOT])

if __name__ == "__main__":
    menu = (
        pystray.MenuItem('Show', launch, default=True),
        pystray.MenuItem('Quit', lambda icon, _: icon.stop())
    )

    icon = pystray.Icon("oshot_main_icon", create_image(), "oshot launcher", menu)

    # Run in a separate thread
    threading.Thread(target=listen_socket, daemon=True).start()

    icon.run()

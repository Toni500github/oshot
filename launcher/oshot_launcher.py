#!/bin/env python3
from time import sleep
import threading
import pystray
import socket
import pyclip
import subprocess
from PIL import Image, ImageDraw

HOST = '127.0.0.1'
PORT = 6016

def create_image():
    # Simple icon for now
    image = Image.new('RGB', (64, 64), color='blue')
    dc = ImageDraw.Draw(image)
    dc.rectangle([16, 16, 48, 48], fill='white')
    return image

def on_quit(icon):
    icon.stop()

def setup(icon):
    icon.visible = True

def listen_socket():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, PORT))
        s.listen()
        conn, addr = s.accept()
        with conn:
            while True:
                sleep(0.1)
                data = conn.recv(2048)
                if data:
                    data_str = data.decode()
                    print(f"Received: {data_str}")
                    if (data_str.startswith("copy_text: ")):
                        pyclip.copy(data_str.removeprefix("copy_text: "))
                        print("Copied!")

def launch():
    subprocess.run(["oshot"])

if __name__ == "__main__":
    menu = (
        pystray.MenuItem('Show', launch),
        pystray.MenuItem('Quit', on_quit)
    )

    icon = pystray.Icon("oshot_main_icon", create_image(), "oshot launcher", menu)

    # Run in a separate thread
    threading.Thread(target=listen_socket, daemon=True).start()

    icon.run()

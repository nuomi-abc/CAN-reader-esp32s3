import socket
import json
from datetime import datetime

LISTEN_IP = "0.0.0.0"
LISTEN_PORT = 8888
OUTPUT_FILE = "car-data.txt"

def main():
    target = "0.0.0.0", 8888
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(target)
    print(f"UDP receive serve is up,listening {LISTEN_IP}:{LISTEN_PORT}")
    print("wait for ESP32S3's messages...\n")

    # enable "a" mode to prevent to cover the data before
    f = open(OUTPUT_FILE, "a", encoding="utf-8")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            recv_time = datetime.now().strftime("%H:%M:%S.%f")[:-3]

            try:
                payload = json.loads(data.decode('utf-8'))
                ts_us = payload.get("ts", 0)
                can_id = payload.get("id", "N/A")
                dlc = payload.get("dlc", 0)
                can_data = payload.get("data", "")

                line = (f"[{recv_time}] from {addr[0]}:{addr[1]} "
                        f"CAN_ID={can_id} DLC={dlc} DATA={can_data} "
                        f"(device timestamp:{ts_us}us)")

                print(line)

                # flush immediately just in case
                f.write(line + "\n")
                f.flush()

            except json.JSONDecodeError:
                print(f"[{recv_time}] recv invalid json data:{data}")

        except KeyboardInterrupt:
            print("\nreceive stopped")
            break
        except Exception as e:
            print(f"recv fail:{e}")

    f.close()
    sock.close()

if __name__ == "__main__":
    main()

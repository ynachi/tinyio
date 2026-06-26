import socket
import threading
import time
import argparse

def test_echo(client_id, host, port, messages_per_client, message_length):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((host, port))
            for i in range(messages_per_client):
                msg = f"Client {client_id} Msg {i}: " + "A" * message_length
                s.sendall(msg.encode('utf-8'))
                
                expected_len = len(msg)
                received_data = b""
                while len(received_data) < expected_len:
                    chunk = s.recv(4096)
                    if not chunk:
                        print(f"Client {client_id} connection closed unexpectedly.")
                        return
                    received_data += chunk
                
                received_msg = received_data.decode('utf-8')
                if received_msg != msg:
                    print(f"Client {client_id} received corrupted data! Expected {msg}, got {received_msg}")
                    return
            print(f"Client {client_id} finished successfully.")
    except Exception as e:
        print(f"Client {client_id} error: {e}")

def main():
    parser = argparse.ArgumentParser(description="Multi-threaded echo client")
    parser.add_argument("--host", default="127.0.0.1", help="Server IP")
    parser.add_argument("--port", type=int, default=8080, help="Server port")
    parser.add_argument("--threads", type=int, default=10, help="Number of threads/clients")
    parser.add_argument("--messages", type=int, default=100, help="Messages per client")
    parser.add_argument("--length", type=int, default=64, help="Message length padding")
    
    args = parser.parse_args()
    
    threads = []
    start_time = time.time()
    
    print(f"Starting {args.threads} threads, sending {args.messages} messages each to {args.host}:{args.port}")
    
    for i in range(args.threads):
        t = threading.Thread(target=test_echo, args=(i, args.host, args.port, args.messages, args.length))
        threads.append(t)
        t.start()
        
    for t in threads:
        t.join()
        
    end_time = time.time()
    print(f"Completed {args.threads * args.messages} messages in {end_time - start_time:.2f} seconds.")

if __name__ == "__main__":
    main()

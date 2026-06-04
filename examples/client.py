import socket

my_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
my_socket.connect(('127.0.0.1', 5555))
while True:
    text = input(': ')
    if text != 'выход':
        my_socket.send(text.encode())
        print(my_socket.recv(1024).decode())
    else:
        break
my_socket.close()
import socket
import threading
import time
import random
import resource

CONNECTIONS_PER_SECOND = 3900
MAXTIME_PER_CONNECTION = 20
MAX_CONCURRENT_CONNECTIONS = 4000
GRANULARITY = 1000000

def run_client():
	global CONNECTIONS_PER_SECOND
	global MAXTIME_PER_CONNECTION
	s = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP)
	time.sleep(random.randint(0, GRANULARITY) * 1.0 / GRANULARITY)
	start_time = time.time()
	s.connect(("127.0.0.1", 12345))
	connected_time = time.time()

	data = s.recv(100)

	if data != "HELLO WORLD\n":
		print "Didn't recv all data"

	start_sleep_time = time.time()
	time.sleep(random.randint(0, GRANULARITY) * MAXTIME_PER_CONNECTION / GRANULARITY)
	stop_sleep_time = time.time()

	if random.randint(0, 3) <= 2:
		s.send("a")

	s.close()
	stop_time = time.time()
	sleep_time = stop_sleep_time - start_sleep_time
	print "Connect time: %f ms, transaction time: %f ms"%(1000*(connected_time - start_time), 1000*(stop_time - connected_time - sleep_time))

cur_lim, max_lim = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (MAX_CONCURRENT_CONNECTIONS, max_lim))

threads = []

for i in range(CONNECTIONS_PER_SECOND):
	thread = threading.Thread(target=run_client)
	threads.append(thread)
	thread.start()

for i in range(CONNECTIONS_PER_SECOND):
	threads[i].join()

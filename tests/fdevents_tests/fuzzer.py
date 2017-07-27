import socket
import select
import time
import random
import resource
import heapq
import errno
import sys
import math

CONNECTIONS_PER_SECOND = 3900		# Use 900 for select backend (test3)
MAXTIME_PER_CONNECTION = 20
MAX_CONCURRENT_CONNECTIONS = 4000	# Use 1000 for select backend (test3)
GRANULARITY = 1000000

def stddev(values):
	avg = sum(values) / len(values)
	sumsq = sum([(v - avg)**2 for v in values])
	return math.sqrt(sumsq / len(values))

class ClientState(object):
	def __init__(self):
		self.state = "WAITING_CONNECT"

	def connected(self):
		self.state = "RECEIVING"
		self.connect_stop_time = time.time()
		self.receive_start_time = time.time()
		self.data = ""

	def received(self):
		self.state = "WAITING_SEND"
		self.receive_stop_time = time.time()
		return time.time() + random.randint(0, GRANULARITY) * MAXTIME_PER_CONNECTION / GRANULARITY

	def elapsedwaitconnect(self):
		self.state = "CONNECTING"
		self.connect_start_time = time.time()

	def elapsedwaitsend(self):
		if random.randint(0, 3) <= 2:
			self.state = "SENDING"
			return True

		self.state = "SUCCESS"
		self.set_elapsed_times()
		return False

	def sent(self):
		self.state = "FINISHED"

	def finished(self):
		self.state = "SUCCESS"
		self.set_elapsed_times()
	
	def errored(self):
		self.state = "ERROR"

	def set_elapsed_times(self):
		self.connect_time = 1000 * (self.connect_stop_time - self.connect_start_time)
		self.receive_time = 1000 * (self.receive_stop_time - self.receive_start_time)

cur_lim, max_lim = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (MAX_CONCURRENT_CONNECTIONS, max_lim))
sockets = [socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP) for i in range(CONNECTIONS_PER_SECOND)]
states = {}
waitsocks = {}
deadlines = []
fd_to_socket = {}

for s in sockets:
	s.setblocking(0)
	fd_to_socket[s.fileno()] = s
	states[s] = ClientState()
	deadline = 1.0 + time.time() + random.randint(0, GRANULARITY) * 10.0 / GRANULARITY

	if deadline not in deadlines:
		heapq.heappush(deadlines, deadline)

	if deadline not in waitsocks:
		waitsocks[deadline] = []

	waitsocks[deadline].append(s)

poll = select.poll()

while len(sockets) > 0:
	if len(deadlines) > 0:
		timeout = deadlines[0] - time.time()
	else:
		timeout = None
	
	events = poll.poll(timeout)

	if len(deadlines) > 0:
		deadline = heapq.heappop(deadlines)

		if deadline in deadlines:
			deadlines.remove(deadline)

		for s in waitsocks[deadline]:
			if states[s].state == "WAITING_CONNECT":
				try:
					s.connect(("127.0.0.1", 12345))
				except socket.error as e:
					if e.errno != errno.EINPROGRESS:
						sys.stderr.write(str(e) + "\n")
						sockets.remove(s)
						s.close()
						continue

				states[s].elapsedwaitconnect()
				poll.register(s, select.POLLOUT)

			elif states[s].state == "WAITING_SEND":
				send_reply = states[s].elapsedwaitsend()
	
				if send_reply == True:
					poll.register(s, select.POLLOUT)
				else:
					sockets.remove(s)
					s.close()

		del waitsocks[deadline]

	for fd, event in events:
		s = fd_to_socket[fd]

		if event & select.POLLIN:
			if states[s].state == "RECEIVING":
				try:
					data = s.recv(1)
				except Exception as e:
					sys.stderr.write("Got exception during recv: %s\n"%str(e))
					states[s].errored()
					poll.unregister(s)
					sockets.remove(s)
					s.close()
					continue

				if len(data) == 0:
					sys.stderr.write("Server closed connection unexpectedly\n")
					states[s].errored()
					poll.unregister(s)
					sockets.remove(s)
					s.close()
					continue
	
				states[s].data += data
	
				if states[s].data == "HELLO WORLD\n":
					deadline = states[s].received()
	
					if deadline not in waitsocks:
						waitsocks[deadline] = []

					if deadline not in deadlines:
						heapq.heappush(deadlines, deadline)

					waitsocks[deadline].append(s)
					poll.unregister(s)
			elif states[s].state == "FINISHED":
				try:
					data = s.recv(1)

					if len(data) > 0:
						sys.stderr.write("Got more data? " + data + "\n")
						states[s].errored()
					else:
						states[s].finished()
				except Exception as e:
					sys.stderr.write("Got exception during recv: %s\n"%str(e))
					states[s].errored()
	
				poll.unregister(s)
				sockets.remove(s)
				s.close()

		elif event & select.POLLOUT:
			if states[s].state == "CONNECTING":
				states[s].connected()
				poll.modify(s, select.POLLIN)
			elif states[s].state == "SENDING":
				s.sendall("a")
				states[s].sent()
				poll.modify(s, select.POLLIN)
		elif event & select.POLLHUP:
			sys.stderr.write("Server closed connection unexpectedly\n")
			states[s].errored()
			poll.unregister(s)
			sockets.remove(s)
			s.close()
		else:
			sys.stderr.write("Error in socket\n")
			states[s].errored()
			poll.unregister(s)
			sockets.remove(s)
			s.close()

connect_times = []
receive_times = []

print "Test finished"

for s in states:
	state = states[s]

	if state.state != "SUCCESS":
		print "Got error"
		quit()

	connect_times.append(state.connect_time)
	receive_times.append(state.receive_time)

print "Avg/Max/Std connect time: %f ms / %f ms / %f ms"%(sum(connect_times) / len(connect_times), max(connect_times), stddev(connect_times))
print "Avg/Max/Std receive time: %f ms / %f ms / %f ms"%(sum(receive_times) / len(receive_times), max(receive_times), stddev(receive_times))

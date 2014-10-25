jackpatch
====

This is a module that exposes some of the functionality of the 
[JACK Audio Connection Kit](http://jackaudio.org/) for use in Python programs.
Since JACK is a realtime audio server and Python isn't going to be the best at
audio processing at realtime speeds, this module only exposes JACK functionality 
that either doesn't need to be realtime (such as managing ports, patchbay 
connections, and the transport) or that can be handled with a queue and poll
architecture (just MIDI send and receive at this point).

Installing
====

The module is written in C using Python's basic API. You'll need JACK installed
with header files in place, a C compiler, and Python setuptools. Once that's in
place you should be able to run this from the directory this file is in:

    sudo python setup.py install

It's been tested on Linux and FreeBSD, and should be very portable, but your 
mileage may vary.

Using
====

This module aims to be as Pythonic as possible, which isn't too hard given 
JACK's brilliantly designed API. Before you can do anything, you'll need to 
create a client that gives your application a presence on the JACK server. The 
client needs a name, which can be whatever you want. You can use the open 
methods to connect the client to the JACK server, but generally you won't need 
to because any other method that requires an open connection will call it for 
you. Similarly, you don't usually need to call close, as things will get 
cleaned up when your program exits. Calling open or close multiple times does
no harm.

```python
import time
import jackpatch

client = jackpatch.Client("superduper")
print(client.name)                       # superduper
print(client.is_open)                    # False
client.open()
print(client.is_open)                    # True
time.sleep(1)
client.close()
print(client.is_open)                    # False
```

Having a client allows you to list and create ports, which are endpoints that 
can be connected to transmit audio or MIDI data between JACK clients. You can 
list existing ports with the get_ports method. Keep in mind that calling this
method again will return a new list of distinct jackpatch.Port instances that 
point to the same ports. You can pass regex pattern strings for the name and 
type of the port, and a set of flags to filter for port characteristics:

| Flag               | Meaning                                                         |
| ------------------ | --------------------------------------------------------------- |
| JackPortIsInput    | if JackPortIsInput is set, then the port can receive data       |
| JackPortIsOutput   | if JackPortIsOutput is set, then data can be read from the port |
| JackPortIsPhysical | the port corresponds to some kind of physical I/O connector     |
| JackPortCanMonitor | the port supports input or output monitoring                    |
| JackPortIsTerminal | the port's data doesn't come from or go to any other port       |

You can also create your own ports directly, passing the client that's going to 
own the port, a name for it, and some of the above flags to indicate what type 
it is. All ports created this way will be of JACK's default MIDI type. These 
ports are then owned by the client and can be listed specially.

```python
import jackpatch

client = jackpatch.Client("superduper")

for port in client.get_ports():
  print(port.name)                       # system:capture1
                                         # system:capture2
                                         # system:playback1
                                         # system:playback2

for port in client.get_ports(flags=jackpatch.JackPortIsOutput):
  print(port.name)                       # system:capture1
                                         # system:capture2

for port in client.get_ports(name_pattern="system:playback.*"):
  print(port.name)                       # system:playback1
                                         # system:playback2

midi_port = jackpatch.Port(client, "midi_in", flags=jackpatch.JackPortIsInput)

for port in client.get_ports(type_pattern=".*midi.*"):
  print(port.name)                       # superduper:midi_in

for port in client.get_ports(mine=True):
  print(port.name)                       # superduper:midi_in

```

Ports can be connected and disconnected by a client, whether they belong to 
that client or not. The order of parameters is important when connecting and 
disconnecting: the first port must be an output port and the second must be 
an input port. Connection ports that are already connected or disconnecting 
ones that aren't connected has no consequences. Any port can list all the ports 
connected to it. Keep in mind that ports listed this way are new objects that 
won't be the same as existing instances.

```python
import jackpatch

clientA = jackpatch.Client("superduper")
midi_out = jackpatch.Port(clientA, "midi_out", flags=jackpatch.JackPortIsOutput)

clientB = jackpatch.Client("looper")
midi_in = jackpatch.Port(clientB, "midi_in", flags=jackpatch.JackPortIsInput)

clientA.connect(midi_out, midi_in)
for port in midi_in.get_connections():
  print(port.name, port is midi_out)    # superduper:midi_out, False

clientA.disconnect(midi_out, midi_in)
print len(midi_in.get_connections())    # 0

```

MIDI events can be sent on an output port at any time, and are queued up for 
sending to JACK at some point in the future. Messages are specified by passing 
a sequence of bytes representing a single complete MIDI message, usually 
three bytes, but more in the case of system exclusive (aka SysEx) or other 
extended messages. You can also pass a delay in seconds, and the message will 
be sent after that amount of time has elapsed. If no delay is passed the 
message will be sent as soon as possible. You can send delayed messages in any
order and they will be sorted in the queue such that they play back in the 
correct order.

```python
import jackpatch

client = jackpatch.Client("superduper")
midi_out = jackpatch.Port(client, "midi_out", flags=jackpatch.JackPortIsOutput)

# this will play a C2 at full velocity for half a second
midi_out.send((0x90, 0x24, 0x7F))
midi_out.send((0x80, 0x24, 0x7F), 0.5)

```

To receive MIDI events on an imput port, you'll generally want to poll 
periodically for messages. Since received messages are tagged with the current
transport time when they were received, you can get excellent time accuracy
without polling very often, but if you want to do something with the messages
right away you can poll more often. The downside to this is that it consumes 
more system resources. If you poll very infrequently, events will build up in 
the queue and consume some memory, but they're generally around 16 bytes and a 
lot of them can be stored before it becomes a problem.

Events are returned as a tuple with the first member being a sequence of 
numeric byte values and the second being the transport time at which the event 
was received in seconds. If the transport isn't rolling, the time will always 
be wherever the transport is stopped, which isn't that useful. Each call to 
receive will return at most one message, so you may need to call it several 
times to empty the queue. When there are no more events on the queue, the 
receive method will return None, signalling that you should probably wait until 
more events can arrive before polling again. The following code implements a 
simple pass-through that sends its input to its output and prints everything 
that passes through it.

```python
import time
import jackpatch

client = jackpatch.Client("superduperlooper")
midi_in = jackpatch.Port(client, "midi_in", flags=jackpatch.JackPortIsInput)
midi_out = jackpatch.Port(client, "midi_out", flags=jackpatch.JackPortIsOutput)

while(True):
  while(True):
    message = midi_in.receive()
    if (message is None): break
    print(message)               # ([0x80, 0x24, 0x7F], 0.0)
    midi_out.send(message[0])
  time.sleep(0.1)

```

JACK also includes a transport, which is basically a device for keeping track 
of a point on a timeline and advancing it at a steady rate. The timeline 
could represent the duration of an audio recording, movie, dance, animation, or 
anything else that has a temporal dimension. This module gives you a class to 
access JACK's transport from any client via the transport attribute. You can 
get and set the current time on the transport, as well as test and control 
whether it's rolling, i.e. advancing automatically.

```python
import time
import jackpatch

client = jackpatch.Client("tapedeck")

# you can skip to any positive location on the transport
print(client.transport.time)          # 0.0
client.transport.time = -1.0
print(client.transport.time)          # 0.0
client.transport.time = 0.5
print(client.transport.time)          # 0.5

# you can start the transport rolling
print(client.transport.is_rolling)    # False
client.transport.start()
print(client.transport.is_rolling)    # True

# here we see that sleep isn't meant to be very accurate
time.sleep(0.5)
print(client.transport.time)          # 0.977416666667
client.transport.stop()
print(client.transport.is_rolling)    # False

# the time advanced even while we stopped the transport above...
print(client.transport.time)          # 0.977520833333
# ...but it doesn't advance when the transport isn't rolling
time.sleep(0.5)
print(client.transport.time)          # 0.977520833333

# the transport can also be started and stopped by setting an attribute,
#  which works identically to the start() and stop() methods
client.transport.is_rolling = True
time.sleep(0.5)
client.transport.is_rolling = False
print(client.transport.time)          # 1.46866666667

```

Finally, things can go wrong at times, even when you do everything right. For 
instance, the JACK server can be unavailable, another client can close without
warning, and so on. In some cases, the module may generate a runtime warning 
and try to continue if that's possible, but most likely it will throw an 
instance of JackError. You can catch this and handle it if you want to. The 
following example demonstrates a repeatable error.

```python
import jackpatch

client = jackpatch.Client("superduperlooper")
midi_in = jackpatch.Port(client, "midi_in", flags=jackpatch.JackPortIsInput)
midi_out = jackpatch.Port(client, "midi_out", flags=jackpatch.JackPortIsOutput)

# this attempts to connect the ports in the wrong order and generates a warning
client.connect(midi_in, midi_out)   # sys:1: RuntimeWarning: Failed to connect JACK ports (error -1)

# this attempts to send MIDI the wrong way and generates an error
try:
  midi_in.send((0x80, 0x24, 0x7F))
except jackpatch.JackError as e:
  print(str(e))                     # Only output ports can send MIDI messages

```

Happy hacking! Bug reports and pull requests are welcome.